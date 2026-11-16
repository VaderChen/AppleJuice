        // D3DX9 數學補充：矩陣（投影、視角、陰影、行列式、分解、組合轉換）、向量（重心、Catmull-Rom、Hermite、2D 轉換、陣列轉換、投影）、
        // 四元數（軸角、Exp／Ln、Slerp、Squad）、平面、色彩、Fresnel、射線測試與球諧乘積／旋轉／光源。
        // 計算交給 DirectXMath（d3dx_math_native／d3dx_sh，ARM NEON 原生）；陰影矩陣與三角形測試依 D3DX 文件公式另寫。
        bool extra=true;
        const auto optional=[&](std::uint64_t p,unsigned count,Vector& v)->const float*{if(!p)return nullptr;v=load(p,count);return v.data();};
        const auto optional_matrix=[&](std::uint64_t p,Matrix& m)->const float*{if(!p)return nullptr;m=matrix(p);return m.data();};
        const auto read_viewport=[&](std::uint64_t p){std::array<float,6> v{};for(unsigned i=0;i<6;++i)v[i]=i<4?static_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{p+4*i},32))):load(p+4*i,1)[0];return v;};
        const auto transform=[&](const Vector& v,const Matrix& m){Vector out{};for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)out[j]+=v[i]*m[i*4+j];return out;};
        const auto sub=[](const Vector& a,const Vector& b){return Vector{a[0]-b[0],a[1]-b[1],a[2]-b[2],a[3]-b[3]};};
        const auto bool_result=[&](bool value){result=value?1U:0U;};
        if(symbol=="D3DXMatrixPerspectiveFovRH" || symbol=="D3DXMatrixPerspectiveLH" || symbol=="D3DXMatrixPerspectiveRH"){
            Matrix m{};d3dx_math::perspective(m.data(),f(1),f(2),f(3),f(4),symbol.find("Fov")!=symbol.npos,symbol.ends_with("RH"));store(arg(0),m,16);
        }else if(symbol=="D3DXMatrixPerspectiveOffCenterLH" || symbol=="D3DXMatrixPerspectiveOffCenterRH" || symbol=="D3DXMatrixOrthoOffCenterRH"){
            Matrix m{};
            if(symbol.starts_with("D3DXMatrixOrtho"))d3dx_math::ortho_off_center_rh(m.data(),f(1),f(2),f(3),f(4),f(5),f(6));
            else d3dx_math::perspective_off_center(m.data(),f(1),f(2),f(3),f(4),f(5),f(6),symbol.ends_with("RH"));
            store(arg(0),m,16);
        }else if(symbol=="D3DXMatrixLookAtRH"){
            Matrix m{};const auto eye=load(arg(1),3),at=load(arg(2),3),up=load(arg(3),3);d3dx_math::look_at_rh(m.data(),eye.data(),at.data(),up.data());store(arg(0),m,16);
        }else if(symbol=="D3DXMatrixShadow"){
            // P＝正規化平面、L＝光源、d＝-dot(P, L)：M[i][j]＝P[i]·L[j]＋(i＝j 時加 d)。
            const auto light=load(arg(1),4);auto plane=load(arg(2),4);const auto length=std::sqrt(dot(plane,plane,3));
            if(length!=0)for(auto& v:plane)v/=length;const auto d=-dot(plane,light,4);Matrix m{};
            for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)m[i*4+j]=plane[i]*light[j]+(i==j?d:0.0f);store(arg(0),m,16);
        }else if(symbol=="D3DXMatrixDeterminant"){
            conversion_result(state,x86_,d3dx_math::determinant(matrix(arg(0)).data()),true);result=0;
        }else if(symbol=="D3DXMatrixDecompose"){
            // (pOutScale, pOutRotation, pOutTranslation, pM)：無法分解（縮放過小）時回 D3DERR_INVALIDCALL。
            Vector s{},q{},t{};
            if(!arg(0) || !arg(1) || !arg(2) || !d3dx_math::decompose(s.data(),q.data(),t.data(),matrix(arg(3)).data()))result=0x8876086cU;
            else {store(arg(0),s,3);store(arg(1),q,4);store(arg(2),t,3);result=0;}
        }else if(symbol=="D3DXMatrixTransformation"){
            Vector a,b,c,d,e,g;Matrix m{};
            d3dx_math::transformation(m.data(),optional(arg(1),3,a),optional(arg(2),4,b),optional(arg(3),3,c),optional(arg(4),3,d),optional(arg(5),4,e),optional(arg(6),3,g));store(arg(0),m,16);
        }else if(symbol=="D3DXMatrixTransformation2D"){
            Vector a,c,d,g;Matrix m{};
            d3dx_math::transformation_2d(m.data(),optional(arg(1),2,a),f(2),optional(arg(3),2,c),optional(arg(4),2,d),f(5),optional(arg(6),2,g));store(arg(0),m,16);
        }else if(symbol=="D3DXMatrixAffineTransformation"){
            Vector a,b,c;Matrix m{};d3dx_math::affine(m.data(),f(1),optional(arg(2),3,a),optional(arg(3),4,b),optional(arg(4),3,c));store(arg(0),m,16);
        }else if(symbol=="D3DXMatrixAffineTransformation2D"){
            Vector a,c;Matrix m{};d3dx_math::affine_2d(m.data(),f(1),optional(arg(2),2,a),f(3),optional(arg(4),2,c));store(arg(0),m,16);
        }else if(symbol.ends_with("BaryCentric") && symbol.starts_with("D3DXVec")){
            const auto n=static_cast<unsigned>(symbol[7]-'0');const auto a=load(arg(1),n),b=load(arg(2),n),c=load(arg(3),n);const auto ff=f(4),gg=f(5);Vector out{};
            for(unsigned i=0;i<n;++i)out[i]=a[i]+ff*(b[i]-a[i])+gg*(c[i]-a[i]);store(arg(0),out,n);
        }else if(symbol.ends_with("CatmullRom")){
            const auto n=static_cast<unsigned>(symbol[7]-'0');const auto v0=load(arg(1),n),v1=load(arg(2),n),v2=load(arg(3),n),v3=load(arg(4),n);const auto s=f(5),s2=s*s,s3=s2*s;Vector out{};
            for(unsigned i=0;i<n;++i)out[i]=0.5f*(2*v1[i]+(v2[i]-v0[i])*s+(2*v0[i]-5*v1[i]+4*v2[i]-v3[i])*s2+(3*v1[i]-v0[i]-3*v2[i]+v3[i])*s3);store(arg(0),out,n);
        }else if(symbol.ends_with("Hermite")){
            const auto n=static_cast<unsigned>(symbol[7]-'0');const auto v1=load(arg(1),n),t1=load(arg(2),n),v2=load(arg(3),n),t2=load(arg(4),n);const auto s=f(5),s2=s*s,s3=s2*s;Vector out{};
            for(unsigned i=0;i<n;++i)out[i]=(2*s3-3*s2+1)*v1[i]+(s3-2*s2+s)*t1[i]+(3*s2-2*s3)*v2[i]+(s3-s2)*t2[i];store(arg(0),out,n);
        }else if(symbol=="D3DXVec2Transform" || symbol=="D3DXVec2TransformCoord" || symbol=="D3DXVec2TransformNormal"){
            auto v=load(arg(1),2);v[2]=0;v[3]=symbol.ends_with("Normal")?0.0f:1.0f;auto out=transform(v,matrix(arg(2)));
            if(symbol.ends_with("Coord")){for(unsigned i=0;i<2;++i)out[i]/=out[3];}
            store(arg(0),out,symbol=="D3DXVec2Transform"?4:2);
        }else if(symbol.ends_with("TransformArray") || symbol.ends_with("TransformCoordArray") || symbol.ends_with("TransformNormalArray")){
            // (pOut, OutStride, pV, VStride, pM, n)：輸入維度與輸出格式依名稱（Vec2／Vec3／Vec4／Plane；一般版輸出 4 維）。
            const unsigned in=symbol.starts_with("D3DXVec2")?2:symbol.starts_with("D3DXVec3")?3:4;const bool coord=symbol.find("Coord")!=symbol.npos,normal=symbol.find("Normal")!=symbol.npos;
            const auto out_stride=static_cast<std::uint32_t>(arg(1)),in_stride=static_cast<std::uint32_t>(arg(3));const auto m=matrix(arg(4));const auto count=static_cast<std::uint32_t>(arg(5));
            const unsigned written=coord || normal?in:4;
            for(std::uint32_t k=0;k<count;++k){
                auto v=load(arg(2)+std::uint64_t{k}*in_stride,in);if(in<4){for(unsigned i=in;i<4;++i)v[i]=0;v[3]=normal?0.0f:1.0f;}
                auto out=transform(v,m);if(coord)for(unsigned i=0;i<in;++i)out[i]/=out[3];
                store(arg(0)+std::uint64_t{k}*out_stride,out,written);
            }
        }else if(symbol=="D3DXVec3Project"){
            Matrix p,v,w;const auto port=read_viewport(arg(2));const auto input=load(arg(1),3);Vector out{};
            d3dx_math::project(out.data(),input.data(),port.data(),optional_matrix(arg(3),p),optional_matrix(arg(4),v),optional_matrix(arg(5),w));store(arg(0),out,3);
        }else if(symbol=="D3DXVec3ProjectArray" || symbol=="D3DXVec3UnprojectArray"){
            // (pOut, OutStride, pV, VStride, pViewport, pProjection, pView, pWorld, n)；矩陣為 NULL 視為單位矩陣。
            const auto port=read_viewport(arg(4));Matrix p=identity(),v=identity(),w=identity();if(arg(5))p=matrix(arg(5));if(arg(6))v=matrix(arg(6));if(arg(7))w=matrix(arg(7));
            const auto out_stride=static_cast<std::uint32_t>(arg(1)),in_stride=static_cast<std::uint32_t>(arg(3));const auto count=static_cast<std::uint32_t>(arg(8));
            for(std::uint32_t k=0;k<count;++k){
                const auto input=load(arg(2)+std::uint64_t{k}*in_stride,3);Vector out{};
                if(symbol=="D3DXVec3ProjectArray")d3dx_math::project(out.data(),input.data(),port.data(),p.data(),v.data(),w.data());
                else d3dx_unproject(out.data(),input.data(),port.data(),p.data(),v.data(),w.data());
                store(arg(0)+std::uint64_t{k}*out_stride,out,3);
            }
        }else if(symbol=="D3DXVec4Cross"){
            const auto a=load(arg(1),4),b=load(arg(2),4),c=load(arg(3),4);Vector out{};d3dx_math::vec4_cross(out.data(),a.data(),b.data(),c.data());store(arg(0),out,4);
        }else if(symbol=="D3DXQuaternionRotationAxis"){
            const auto axis=load(arg(1),3);Vector q{};d3dx_math::quaternion_rotation_axis(q.data(),axis.data(),f(2));store(arg(0),q,4);
        }else if(symbol=="D3DXQuaternionToAxisAngle"){
            // 軸取 xyz（不正規化）、角度 2·acos(w)；回傳 void。
            const auto q=load(arg(0),4);if(arg(1))store(arg(1),q,3);
            if(arg(2))memory.store(GuestAddress{arg(2)},32,std::bit_cast<std::uint32_t>(2*std::acos(std::clamp(q[3],-1.0f,1.0f))));result=0;
        }else if(symbol=="D3DXQuaternionExp" || symbol=="D3DXQuaternionLn"){
            const auto input=load(arg(1),4);Vector q{};if(symbol.ends_with("Exp"))d3dx_math::quaternion_exp(q.data(),input.data());else d3dx_math::quaternion_ln(q.data(),input.data());store(arg(0),q,4);
        }else if(symbol=="D3DXQuaternionSlerp"){
            const auto a=load(arg(1),4),b=load(arg(2),4);Vector q{};d3dx_math::quaternion_slerp(q.data(),a.data(),b.data(),f(3));store(arg(0),q,4);
        }else if(symbol=="D3DXQuaternionSquad"){
            const auto q1=load(arg(1),4),a=load(arg(2),4),b=load(arg(3),4),c=load(arg(4),4);Vector q{};d3dx_math::quaternion_squad(q.data(),q1.data(),a.data(),b.data(),c.data(),f(5));store(arg(0),q,4);
        }else if(symbol=="D3DXQuaternionSquadSetup"){
            const auto q0=load(arg(3),4),q1=load(arg(4),4),q2=load(arg(5),4),q3=load(arg(6),4);Vector a{},b{},c{};
            d3dx_math::quaternion_squad_setup(a.data(),b.data(),c.data(),q0.data(),q1.data(),q2.data(),q3.data());store(arg(0),a,4);store(arg(1),b,4);store(arg(2),c,4);result=0;
        }else if(symbol=="D3DXQuaternionBaryCentric"){
            const auto q1=load(arg(1),4),q2=load(arg(2),4),q3=load(arg(3),4);Vector q{};d3dx_math::quaternion_barycentric(q.data(),q1.data(),q2.data(),q3.data(),f(4),f(5));store(arg(0),q,4);
        }else if(symbol=="D3DXPlaneFromPoints"){
            const auto a=load(arg(1),3),b=load(arg(2),3),c=load(arg(3),3);auto n=norm(cross(sub(b,a),sub(c,a)));n[3]=-dot(n,a);store(arg(0),n,4);
        }else if(symbol=="D3DXPlaneIntersectLine"){
            // 直線與平面平行時回 NULL，輸出不變。
            const auto plane=load(arg(1),4),a=load(arg(2),3),b=load(arg(3),3);Vector out{};
            if(d3dx_math::plane_intersect_line(out.data(),plane.data(),a.data(),b.data()))store(arg(0),out,3);else result=0;
        }else if(symbol=="D3DXPlaneNormalize"){
            auto plane=load(arg(1),4);const auto length=std::sqrt(dot(plane,plane,3));for(auto& v:plane)v=length!=0?v/length:0.0f;store(arg(0),plane,4);
        }else if(symbol=="D3DXPlaneTransform"){
            store(arg(0),transform(load(arg(1),4),matrix(arg(2))),4);
        }else if(symbol=="D3DXColorAdjustContrast" || symbol=="D3DXColorAdjustSaturation"){
            const auto color=load(arg(1),4);Vector out{};
            if(symbol.ends_with("Contrast"))d3dx_math::color_contrast(out.data(),color.data(),f(2));else d3dx_math::color_saturation(out.data(),color.data(),f(2));store(arg(0),out,4);
        }else if(symbol=="D3DXFresnelTerm"){
            conversion_result(state,x86_,d3dx_math::fresnel(f(0),f(1)),true);result=0;
        }else if(symbol=="D3DXIntersectTri"){
            // (p0, p1, p2, rayPos, rayDir, pU, pV, pDist)：命中點＝p0＋U·(p1−p0)＋V·(p2−p0)，只接受射線前方（距離 ≥ 0）。
            const auto p0=load(arg(0),3),p1=load(arg(1),3),p2=load(arg(2),3),origin=load(arg(3),3),direction=load(arg(4),3);
            const auto e1=sub(p1,p0),e2=sub(p2,p0),p=cross(direction,e2);const auto det=dot(e1,p);bool hit=false;float u=0,v=0,t=0;
            if(det!=0){const auto inv=1/det;const auto s=sub(origin,p0);u=dot(s,p)*inv;
                if(u>=0 && u<=1){const auto q=cross(s,e1);v=dot(direction,q)*inv;if(v>=0 && u+v<=1){t=dot(e2,q)*inv;hit=t>=0;}}}
            if(hit){if(arg(5))store(arg(5),Vector{u},1);if(arg(6))store(arg(6),Vector{v},1);if(arg(7))store(arg(7),Vector{t},1);}
            bool_result(hit);
        }else if(symbol=="D3DXBoxBoundProbe"){
            const auto lo=load(arg(0),3),hi=load(arg(1),3),origin=load(arg(2),3),direction=load(arg(3),3);
            float near=-std::numeric_limits<float>::infinity(),far=std::numeric_limits<float>::infinity();bool hit=true;
            for(unsigned i=0;i<3 && hit;++i){
                if(direction[i]==0){if(origin[i]<lo[i] || origin[i]>hi[i])hit=false;continue;}
                auto t1=(lo[i]-origin[i])/direction[i],t2=(hi[i]-origin[i])/direction[i];if(t1>t2)std::swap(t1,t2);
                near=std::max(near,t1);far=std::min(far,t2);if(near>far)hit=false;
            }
            bool_result(hit && far>=0);
        }else if(symbol=="D3DXSphereBoundProbe"){
            const auto center=load(arg(0),3),origin=load(arg(2),3),direction=load(arg(3),3);const auto radius=f(1);
            const auto offset=sub(origin,center);const auto a=dot(direction,direction),b=2*dot(direction,offset),c=dot(offset,offset)-radius*radius;
            const auto disc=b*b-4*a*c;bool hit=false;
            if(c<=0)hit=true;else if(a!=0 && disc>=0){const auto root=std::sqrt(disc);hit=(-b-root)/(2*a)>=0 || (-b+root)/(2*a)>=0;}
            bool_result(hit);
        }else if(symbol=="D3DXSHMultiply2" || symbol=="D3DXSHMultiply3" || symbol=="D3DXSHMultiply4"){
            const unsigned order=static_cast<unsigned>(symbol.back()-'0'),count=order*order;std::array<float,36> a{},b{},out{};
            for(unsigned i=0;i<count;++i){a[i]=load(arg(1)+4*i,1)[0];b[i]=load(arg(2)+4*i,1)[0];}
            (void)sh_multiply(out.data(),order,a.data(),b.data());store(arg(0),out,count);
        }else if(symbol=="D3DXSHRotateZ"){
            // (pOut, Order, Angle, pIn)：繞 Z 軸旋轉，與 D3DXSHRotate 共用 SH 基底。
            const auto order=static_cast<unsigned>(arg(1));
            if(order>=2 && order<=6){std::array<float,36> in{},out{};for(unsigned i=0;i<order*order;++i)in[i]=load(arg(3)+4*i,1)[0];sh_rotate_z(out.data(),order,f(2),in.data());store(arg(0),out,order*order);}
        }else if(symbol=="D3DXSHEvalDirectionalLight" || symbol=="D3DXSHEvalSphericalLight" || symbol=="D3DXSHEvalHemisphereLight"){
            // 方向光 (Order, pDir, R, G, B, pR, pG, pB)；球形光 (Order, pPos, Radius, R, G, B, pR, pG, pB)；
            // 半球光 (Order, pDir, Top, Bottom（D3DXCOLOR 以值傳遞）, pR, pG, pB)。pG／pB 可為 NULL。
            const auto order=static_cast<unsigned>(arg(0));const bool spherical=symbol.find("Spherical")!=symbol.npos,hemisphere=symbol.find("Hemisphere")!=symbol.npos;
            const unsigned first=hemisphere?4U:spherical?6U:5U;std::array<float,36> r{},g{},b{};
            if(!arg(1) || !arg(first) || order<2 || order>6)result=0x8876086cU;
            else{
                const auto vector=load(arg(1),3);bool ok=false;
                if(hemisphere){const auto top=load(arg(2),4),bottom=load(arg(3),4);ok=sh_hemisphere(order,vector.data(),top.data(),bottom.data(),r.data(),arg(5)?g.data():nullptr,arg(6)?b.data():nullptr);}
                else {const Vector color{f(spherical?3:2),f(spherical?4:3),f(spherical?5:4),0};
                    ok=spherical?sh_spherical(order,vector.data(),f(2),color.data(),r.data(),arg(7)?g.data():nullptr,arg(8)?b.data():nullptr):
                                 sh_directional(order,vector.data(),color.data(),r.data(),arg(6)?g.data():nullptr,arg(7)?b.data():nullptr);}
                const auto count=order*order;
                if(!ok)result=0x8876086cU;
                else {store(arg(first),r,count);if(arg(first+1))store(arg(first+1),g,count);if(arg(first+2))store(arg(first+2),b,count);result=0;}
            }
        }else if(symbol=="D3DXCheckVersion"){
            // 所有 d3dx9_24～43 共用同一實作：接受 D3D_SDK_VERSION 31／32 與該範圍內的 D3DX_SDK_VERSION。
            const auto d3d=static_cast<std::uint32_t>(arg(0)),d3dx=static_cast<std::uint32_t>(arg(1));bool_result((d3d==31 || d3d==32) && d3dx>=24 && d3dx<=43);
        }else if(symbol=="D3DXDebugMute"){
            // 偵錯輸出靜音旗標（每個程序一份，位於 CRT 資料區）；回傳先前的狀態。
            const GuestAddress cell{runtime_base_.value+200};result=memory.load(cell,32);memory.store(cell,32,static_cast<std::uint32_t>(arg(0))?1U:0U);
        }else extra=false;
        if(extra){arg.return_to(state,result);return std::nullopt;}
