#include "x64arm/ole_drag.hpp"

namespace x64arm::ole_drag {
namespace {constexpr bool failed(std::uint32_t status){return (status&0x80000000U)!=0;}}
void Session::issue(Action action,std::uint32_t effect){
    request_={action,held_,input_,effect};
    if(action==Action::QueryContinue || action==Action::Feedback || action==Action::DefaultFeedback || action==Action::Wait || action==Action::Finish)request_.target={};
}
bool Session::begin(const Input& input){
    if(request_.action!=Action::Wait)return false;
    input_=input;if(!input_.target)input_.target={};
    intent_=Intent::Continue;cancel_pending_=false;invalidated_=false;drop_started_=false;
    issue(Action::QueryContinue);return true;
}
void Session::cancel(std::uint32_t status){
    // Drop 已交給接收者後，不能用稍晚的輸入撤銷已交付的資料。
    if(request_.action==Action::Finish || drop_started_)return;
    if(status==0 || status==drop)status=cancelled;
    cancel_pending_=true;result_={status,0};
    if(request_.action==Action::Wait)apply_cancellation();
}
void Session::invalidate(const Target& target){
    if(!target || request_.action==Action::Finish || drop_started_)return;
    if(input_.target==target)input_.target={};
    if(held_==target){
        invalidated_=true;
        if(request_.action==Action::Wait)close(Cleanup::Switch);
    }
}
void Session::apply_cancellation(){
    intent_=Intent::Finish;cancel_pending_=false;negotiated_=0;close(Cleanup::Finish);
}
void Session::attach(){
    invalidated_=false;
    if(input_.target){held_=input_.target;issue(Action::AcquireTarget);}
    else {negotiated_=0;if(intent_==Intent::Drop)finish_drop();else {feedback_next_=FeedbackNext::Wait;issue(Action::Feedback);}}
}
void Session::select(){
    if(held_!=input_.target || invalidated_){close(Cleanup::Switch);return;}
    if(intent_==Intent::Drop){finish_drop();return;}
    if(accepted_)issue(Action::Over,allowed_);
    else {negotiated_=0;feedback_next_=FeedbackNext::Wait;issue(Action::Feedback);}
}
void Session::close(Cleanup reason){
    cleanup_=reason;
    if(accepted_)issue(Action::Leave);
    else if(held_reference_)issue(Action::ReleaseTarget);
    else {held_={};after_release();}
}
void Session::after_release(){
    held_={};held_reference_=false;accepted_=false;invalidated_=false;negotiated_=0;
    if(cancel_pending_){apply_cancellation();return;}
    switch(cleanup_){
    case Cleanup::Switch:attach();break;
    case Cleanup::Reject:
        if(intent_==Intent::Drop)finish_drop();
        else {feedback_next_=FeedbackNext::Wait;issue(Action::Feedback);}
        break;
    case Cleanup::Finish:finish();break;
    }
}
void Session::after_feedback(){
    if(feedback_next_==FeedbackNext::Drop)finish_drop();else issue(Action::Wait);
}
void Session::finish_drop(){
    if(accepted_ && (negotiated_&effects)){
        drop_started_=true;issue(Action::Drop,allowed_);return;
    }
    result_={drop,0};intent_=Intent::Finish;close(Cleanup::Finish);
}
void Session::finish(){
    result_.effect&=effects;issue(Action::Finish,result_.effect);
}
bool Session::complete(const Reply& reply){
    const auto action=request_.action;
    if(action==Action::Wait || action==Action::Finish)return false;
    if(reply.transport_error){
        // 未開始的 AddRef 沒有取得引用；Release 無法派送時，仍結束該傳輸工作。
        if(action==Action::AcquireTarget)held_={};
        if(action==Action::ReleaseTarget){held_reference_=false;held_={};}
        if(action==Action::AcquireTarget || action==Action::Enter || action==Action::Leave || action==Action::Drop || action==Action::ReleaseTarget)accepted_=false;
        result_={reply.transport_error,0};intent_=Intent::Finish;
        cancel_pending_=false;close(Cleanup::Finish);return true;
    }
    // 先提交已執行 callback 的引用／進入狀態，再清理回呼期間鎖存的取消。
    if(action==Action::AcquireTarget)held_reference_=true;
    if(action==Action::Enter)accepted_=reply.status==0;
    if(action==Action::Leave)accepted_=false;
    if(action==Action::ReleaseTarget)held_reference_=false;
    if(cancel_pending_){apply_cancellation();return true;}
    if(invalidated_ && action!=Action::QueryContinue && action!=Action::Leave && action!=Action::ReleaseTarget){close(Cleanup::Switch);return true;}
    switch(action){
    case Action::QueryContinue:
        if(reply.status==0 || reply.status==drop){intent_=reply.status==drop?Intent::Drop:Intent::Continue;select();}
        else {result_={reply.status,0};intent_=Intent::Finish;close(Cleanup::Finish);}
        break;
    case Action::AcquireTarget:issue(Action::Enter,allowed_);break;
    case Action::Enter:
        if(!accepted_){input_.target={};close(Cleanup::Reject);}
        else {negotiated_=reply.effect&allowed_;feedback_next_=intent_==Intent::Drop?FeedbackNext::Drop:FeedbackNext::Wait;issue(Action::Feedback,negotiated_);}
        break;
    case Action::Over:
        negotiated_=failed(reply.status)?0:reply.effect&allowed_;
        feedback_next_=FeedbackNext::Wait;issue(Action::Feedback,negotiated_);break;
    case Action::Leave:
        if(held_reference_)issue(Action::ReleaseTarget);else after_release();break;
    case Action::ReleaseTarget:after_release();break;
    case Action::Feedback:
        // GiveFeedback 的成功結果選擇來源自訂游標或系統預設游標。
        if(reply.status==default_cursors)issue(Action::DefaultFeedback,negotiated_);
        else if(failed(reply.status)){result_={reply.status,0};intent_=Intent::Finish;close(Cleanup::Finish);}
        else after_feedback();
        break;
    case Action::DefaultFeedback:after_feedback();break;
    case Action::Drop:
        accepted_=false;intent_=Intent::Finish;
        result_={failed(reply.status) || reply.status==cancelled?reply.status:drop,failed(reply.status) || reply.status==cancelled?0U:reply.effect&allowed_&effects};
        close(Cleanup::Finish);break;
    case Action::Wait:case Action::Finish:break;
    }
    return true;
}
bool Session::valid() const {
    if(accepted_ && !held_reference_)return false;
    if(held_reference_ && !held_)return false;
    if(negotiated_&~allowed_)return false;
    if(result_.effect&~effects)return false;
    if(request_.action==Action::Finish && (held_reference_ || accepted_ || held_))return false;
    if(request_.action==Action::Enter && (!held_reference_ || !held_))return false;
    if((request_.action==Action::Over || request_.action==Action::Leave || request_.action==Action::Drop) && !accepted_)return false;
    if(request_.action==Action::ReleaseTarget && !held_reference_)return false;
    return true;
}
}
