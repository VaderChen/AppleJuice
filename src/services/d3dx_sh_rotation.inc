            const auto source=arg(3), target=arg(0);
            const auto read_coeff=[&](unsigned i){return load(source+4*i,1)[0];};
            const auto write_coeff=[&](unsigned i,float value){
                memory.store(GuestAddress{target+4*i},32,std::bit_cast<std::uint32_t>(value));
            };
            const auto order=static_cast<unsigned>(arg(1));
            write_coeff(0,read_coeff(0));
            if(symbol=="D3DXSHRotateZ"){
                const auto bands=std::clamp(order,2U,6U);
                const auto angle=f(2);
                std::array<float,6> cosine{},sine{};
                for(unsigned l=1;l<bands;++l){
                    cosine[l]=std::cos(static_cast<float>(l)*angle);
                    sine[l]=std::sin(static_cast<float>(l)*angle);
                    const unsigned center=l*(l+1);
                    // D3DX 依序寫入負次、中央與正次係數。輸出覆蓋輸入時，
                    // 同一對中的後續讀取可見先前的清零、乘積與累加。
                    for(unsigned m=l;m>0;--m){
                        const auto low=center-m, high=center+m;
                        if(m!=l)write_coeff(low,0);
                        const float first=cosine[m]*read_coeff(low);
                        write_coeff(low,first);
                        write_coeff(low,first+sine[m]*read_coeff(high));
                    }
                    write_coeff(center,source==target?0.0f:read_coeff(center));
                    for(unsigned m=1;m<=l;++m){
                        const auto low=center-m, high=center+m;
                        if(m!=l)write_coeff(high,0);
                        const float first=-sine[m]*read_coeff(low);
                        write_coeff(high,first);
                        write_coeff(high,first+cosine[m]*read_coeff(high));
                    }
                }
            }else if(order>=2 && order<=6){
                const auto rotation_matrix=matrix(arg(2));
                const auto count=order*order;
                const bool overlap=std::max(source,target)-std::min(source,target)<4*count;
                std::array<float,36> input{},rotated{};
                if(order<=3 && overlap){
                    // 一階逐列寫入。二階先寫對角乘積，其餘欄再依序累加。
                    // 以既有旋轉核心投影單位基底取得權重，保持矩陣慣例一致。
                    for(unsigned row=1;row<4;++row){
                        for(unsigned col=1;col<4;++col)input[col]=read_coeff(col);
                        sh_rotate(rotated.data(),2,rotation_matrix.data(),input.data());
                        write_coeff(row,rotated[row]);
                    }
                    if(order==3){
                        std::array<std::array<float,36>,5> columns{};
                        input.fill(0);
                        for(unsigned col=4;col<9;++col){
                            input[col]=1;
                            sh_rotate(columns[col-4].data(),3,rotation_matrix.data(),input.data());
                            input[col]=0;
                        }
                        for(unsigned row=4;row<9;++row){
                            float value=columns[row-4][row]*read_coeff(row);
                            write_coeff(row,value);
                            for(unsigned col=4;col<9;++col)if(col!=row){
                                value+=columns[col-4][row]*read_coeff(col);
                                write_coeff(row,value);
                            }
                        }
                    }
                }else{
                    for(unsigned i=0;i<count;++i)input[i]=read_coeff(i);
                    sh_rotate(rotated.data(),order,rotation_matrix.data(),input.data());
                    store(target,rotated,count);
                }
            }
