// opengl32 求值器（由 windows_opengl.cpp 的 invoke_opengl 引入）：glMap1／2、glMapGrid1／2、glEvalCoord1／2（含 v 變體）、glEvalMesh1／2、
// glEvalPoint1／2、glGetMapdv／fv／iv。f 與 d 變體在此統一換成雙精度；控制點依 stride 讀出後以 [u][v][分量] 緊密排列交給後端，
// 讓 display list 記錄的是呼叫當下的副本。求值本身由後端完成（Metal 自行求 Bézier，宿主 OpenGL 直接轉給系統實作）。
if(symbol.starts_with("glMap") || symbol.starts_with("glEval") || symbol.starts_with("glGetMap")){
    const bool wide=symbol.ends_with("d") || symbol.ends_with("dv");const unsigned unit=wide?8U:4U;
    const auto real=[&](unsigned i)->double{const auto raw=floating_argument(i,wide);return wide?std::bit_cast<double>(raw):static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));};
    const auto stored=[&](std::uint64_t address)->double{const auto raw=memory.load(GuestAddress{address},static_cast<std::uint8_t>(unit*8));return wide?std::bit_cast<double>(raw):static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));};
    const auto bits=[](double value){return std::bit_cast<std::uint64_t>(value);};
    const auto run=[&](GlOperation operation,std::span<const std::uint64_t> values,std::span<const std::uint8_t> data,std::span<std::uint8_t> out)->std::uint64_t{
        try{return c.native->execute(operation,values,data,out);}catch(const std::bad_alloc&){error(c,out_of_memory);return 0;}};
    auto& points=c.input_scratch;points.clear();
    if(symbol=="glMap1f" || symbol=="glMap1d" || symbol=="glMap2f" || symbol=="glMap2d"){
        // glMap1(target,u1,u2,stride,order,points)；glMap2(target,u1,u2,ustride,uorder,v1,v2,vstride,vorder,points)。
        const bool two=symbol[5]=='2';const auto target=d(0);const auto k=gl_map_components(target);
        if(!k || two!=(target>=0xdb0)){error(c,invalid_enum);return finish(0);}
        const auto ustride=n(3),uorder=n(4),vstride=two?n(7):static_cast<std::int32_t>(k),vorder=two?n(8):1;
        const double u1=real(1),u2=real(2),v1=two?real(5):0.,v2=two?real(6):1.;
        const auto limit=static_cast<std::int32_t>(gl_max_eval_order);
        if(ustride<static_cast<std::int32_t>(k) || vstride<static_cast<std::int32_t>(k) || uorder<1 || uorder>limit || vorder<1 || vorder>limit || u1==u2 || v1==v2){error(c,invalid_value);return finish(0);}
        const auto source=arg(two?9:5);points.resize(static_cast<std::size_t>(uorder)*static_cast<std::size_t>(vorder)*k*8);
        for(std::int32_t i=0;i<uorder;++i)for(std::int32_t j=0;j<vorder;++j)for(unsigned m=0;m<k;++m){
            const auto offset=static_cast<std::uint64_t>(i)*static_cast<std::uint64_t>(ustride)+static_cast<std::uint64_t>(j)*static_cast<std::uint64_t>(two?vstride:0)+m;
            const double value=stored(source+offset*unit);std::memcpy(points.data()+((static_cast<std::size_t>(i)*static_cast<std::size_t>(vorder)+static_cast<std::size_t>(j))*k+m)*8,&value,8);
        }
        if(two){const std::array<std::uint64_t,7> values{target,bits(u1),bits(u2),static_cast<std::uint32_t>(uorder),bits(v1),bits(v2),static_cast<std::uint32_t>(vorder)};run(GlOperation::Map2d,values,points,{});}
        else {const std::array<std::uint64_t,4> values{target,bits(u1),bits(u2),static_cast<std::uint32_t>(uorder)};run(GlOperation::Map1d,values,points,{});}
        return finish(0);
    }
    if(symbol=="glMapGrid1f" || symbol=="glMapGrid1d"){
        if(n(0)<=0){error(c,invalid_value);return finish(0);}const std::array<std::uint64_t,3> values{d(0),bits(real(1)),bits(real(2))};run(GlOperation::MapGrid1d,values,{},{});return finish(0);
    }
    if(symbol=="glMapGrid2f" || symbol=="glMapGrid2d"){
        if(n(0)<=0 || n(3)<=0){error(c,invalid_value);return finish(0);}
        const std::array<std::uint64_t,6> values{d(0),bits(real(1)),bits(real(2)),d(3),bits(real(4)),bits(real(5))};run(GlOperation::MapGrid2d,values,{},{});return finish(0);
    }
    if(symbol.starts_with("glEvalCoord")){
        const bool two=symbol[11]=='2',vector=symbol.ends_with("v");
        const double u=vector?stored(arg(0)):real(0),v=two?(vector?stored(arg(0)+unit):real(1)):0.;
        if(two){const std::array<std::uint64_t,2> values{bits(u),bits(v)};run(GlOperation::EvalCoord2d,values,{},{});}
        else {const std::array<std::uint64_t,1> values{bits(u)};run(GlOperation::EvalCoord1d,values,{},{});}
        return finish(0);
    }
    if(symbol=="glEvalMesh1"){const std::array<std::uint64_t,3> values{d(0),d(1),d(2)};run(GlOperation::EvalMesh1,values,{},{});return finish(0);}
    if(symbol=="glEvalMesh2"){const std::array<std::uint64_t,5> values{d(0),d(1),d(2),d(3),d(4)};run(GlOperation::EvalMesh2,values,{},{});return finish(0);}
    if(symbol=="glEvalPoint1"){const std::array<std::uint64_t,1> values{d(0)};run(GlOperation::EvalPoint1,values,{},{});return finish(0);}
    if(symbol=="glEvalPoint2"){const std::array<std::uint64_t,2> values{d(0),d(1)};run(GlOperation::EvalPoint2,values,{},{});return finish(0);}
    if(symbol=="glGetMapdv" || symbol=="glGetMapfv" || symbol=="glGetMapiv"){
        // GL_COEFF 0x0A00、GL_ORDER 0x0A01、GL_DOMAIN 0x0A02；整數版依規格四捨五入。
        const auto target=d(0),query=d(1);if(!gl_map_components(target) || query<0xa00 || query>0xa02){error(c,invalid_enum);return finish(0);}
        auto& result=c.output_scratch;result.assign(static_cast<std::size_t>(gl_max_eval_order)*gl_max_eval_order*4*8,0);
        const std::array<std::uint64_t,2> values{target,query};const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(run(GlOperation::GetMapdv,values,{},result),result.size()/8));
        const unsigned width=symbol=="glGetMapdv"?8U:4U;
        for(std::size_t i=0;i<count;++i){double value;std::memcpy(&value,result.data()+i*8,8);
            const std::uint64_t raw=symbol=="glGetMapdv"?bits(value):symbol=="glGetMapfv"?std::bit_cast<std::uint32_t>(static_cast<float>(value)):static_cast<std::uint32_t>(gl_integer(value,false));
            memory.store(GuestAddress{arg(2)+i*width},static_cast<std::uint8_t>(width*8),raw);}
        return finish(0);
    }
}
