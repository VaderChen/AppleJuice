                        if(vertex_blending){
                            Vector blended_position{},blended_normal{};float remaining=1;unsigned packed_bones=0;if(blend_indexed)packed_bones=word(bytes,at+12+(betas-1)*4);
                            for(unsigned influence=0;influence<=blend_weights;++influence){
                                const auto weight=influence==blend_weights?remaining:real(word(bytes,at+12+influence*4));if(influence<blend_weights)remaining-=weight;
                                unsigned bone=influence;if(blend_indexed){const unsigned shift=(object.fvf&0x8000)?(influence==0?16U:influence==1?8U:influence==2?0U:24U):influence*8;bone=(packed_bones>>shift)&255;}
                                const auto found=object.transforms.find(256+bone);const auto matrix=found==object.transforms.end()?identity():found->second;const auto transformed=transform(position,matrix);for(unsigned c=0;c<4;++c)blended_position[c]+=weight*transformed[c];
                                const double determinant=double(matrix[0])*(double(matrix[5])*matrix[10]-double(matrix[6])*matrix[9])-double(matrix[1])*(double(matrix[4])*matrix[10]-double(matrix[6])*matrix[8])+double(matrix[2])*(double(matrix[4])*matrix[9]-double(matrix[5])*matrix[8]);
                                if(determinant!=0)for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r){const auto r1=(r+1)%3,r2=(r+2)%3,c1=(c+1)%3,c2=(c+2)%3;blended_normal[c]+=static_cast<float>(weight*normal[r]*(double(matrix[r1*4+c1])*matrix[r2*4+c2]-double(matrix[r1*4+c2])*matrix[r2*4+c1])/determinant);}
                            }position=blended_position;normal=blended_normal;
                        }
