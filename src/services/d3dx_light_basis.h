#pragma once
#define SH_LIGHT_PI 3.14159265358979323846f

/* 實球諧基底依 l*(l+1)+m 排列。輸入方向沿用 D3DX 的座標值；
   不正規化，因而也保留非單位向量的多項式結果。 */
static inline void sh_light_basis(float *out, unsigned order, float x, float y, float z)
{
    /* 含 Condon–Shortley 相位的正規化因子：
       (-1)^m sqrt((2l+1)(l-m)!/(4π(l+m)!) × (m ? 2 : 1))。 */
    static const double norm[6][6] = {
        {0.28209479177387814},
        {0.48860251190291992, -0.48860251190291992},
        {0.63078313050504009, -0.36418281019735971, 0.18209140509867985},
        {0.7463526651802308, -0.30469719964297715, 0.096353714754685141, -0.0393362393284429},
        {0.84628437532163447, -0.26761861742291565, 0.063078313050504001, -0.016858388283618388, 0.0059603403376112018},
        {0.9356025796273888, -0.24157154730437169, 0.045652731285460227, -0.0093188247511476266, 0.0021964680580751762, -0.00069458418713245519},
    };
    double re = 1, im = 0, diagonal = 1;
    for (unsigned m = 0; m < order; ++m) {
        double previous = 0, current = diagonal;
        for (unsigned l = m; l < order; ++l) {
            const unsigned center = l * (l + 1);
            const double weight = norm[l][m] * current;
            out[center + m] = (float)(weight * re);
            if (m) out[center - m] = (float)(weight * im);
            const double next = ((2*l+1)*current*z - (l+m)*previous) / (l+1-m);
            previous = current;
            current = next;
        }
        const double next_re = re*x - im*y;
        im = re*y + im*x;
        re = next_re;
        diagonal *= 2*m+1;
    }
}

static inline float sh_light_cosine_norm(unsigned order)
{
    return (0.75f + (order > 2 ? 0.3125f : 0) - (order > 4 ? 0.09375f : 0)) / SH_LIGHT_PI;
}

static inline float sh_light_hemisphere(unsigned long long i, const float *basis, float top, float bottom)
{
    if (i >= 4) return 0;
    return basis[i] * (i ? (top-bottom)*SH_LIGHT_PI : (top+bottom)*(3*SH_LIGHT_PI));
}

/* 2π ∫ P_l(t) dt（cosθ 到 1）；用 Legendre 遞迴取得導數。
   zeroth 為 1-cosθ，另傳入可避免小角度相減消去有效位數。 */
static inline void sh_light_cap(float *weights,unsigned order,double x,double sin_squared,
                                double zeroth,double norm)
{
    weights[0]=(float)(2*SH_LIGHT_PI*zeroth/norm);
    double previous=1,current=x,prev_derivative=0,derivative=1;
    for(unsigned l=1;l<order;++l){
        weights[l]=(float)(2*SH_LIGHT_PI*sin_squared*derivative/(l*(l+1)*norm));
        const double next=((2*l+1)*x*current-l*previous)/(l+1);
        const double next_derivative=((2*l+1)*(current+x*derivative)-l*prev_derivative)/(l+1);
        previous=current;current=next;prev_derivative=derivative;derivative=next_derivative;
    }
}

/* 球心距離決定張角，半徑一律取絕對值。內含原點時取半球。 */
static inline void sh_light_sphere(float *weights,unsigned order,float distance,float radius)
{
    const double sine=distance<=radius?1.0:(double)radius/distance;
    const double cosine=__builtin_sqrt((1-sine)*(1+sine));
    sh_light_cap(weights,order,cosine,sine*sine,sine*sine/(1+cosine),1);
}
