#include "game_libm.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

float fb(std::uint32_t v){return std::bit_cast<float>(v);}
std::uint32_t bf(float v){return std::bit_cast<std::uint32_t>(v);}
float ftz(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float add(float a,float b){return ftz(a+b);}
float sub(float a,float b){return ftz(a-b);}
float mul(float a,float b){return ftz(a*b);}
// div.s: a zero divisor gives +-FLT_MAX (EE behaviour as the interpreter models it).
float dvd(float a,float b){
    if(b==0)return std::copysign(std::numeric_limits<float>::max(),(a!=0?(std::signbit(a)!=std::signbit(b)?-1.0F:1.0F):1.0F));
    return ftz(a/b);}
// atanf 001212e0 (tables 00499be8 atanhi, 00499bf8 atanlo, 00499c08 aT).
float atanf_impl(float x){
    static const float atanhi[]={fb(0x3eed6338U),fb(0x3f490fdaU),fb(0x3f7b985dU),fb(0x3fc90fdaU)};
    static const float atanlo[]={fb(0x31ac3769U),fb(0x33222167U),fb(0x33140fb4U),fb(0x33a22168U)};
    static const float aT[]={fb(0x3eaaaaabU),fb(0xbe4cccccU),fb(0x3e124924U),fb(0xbde38e38U),fb(0x3dba2e6eU),fb(0xbd9d8795U),
        fb(0x3d886b34U),fb(0xbd6ef16aU),fb(0x3d4bda59U),fb(0xbd15a221U),fb(0x3c8569d7U)};
    const std::int32_t hx=static_cast<std::int32_t>(bf(x));const std::int32_t ix=hx&0x7fffffff;int id;
    if(ix>=0x50800000){
        if(ix>0x7f800000)throw std::runtime_error("atanf NaN");
        return hx>0?add(atanhi[3],atanlo[3]):sub(-atanhi[3],atanlo[3]);
    }
    if(ix<0x3ee00000){
        if(ix<0x31000000)return x;
        id=-1;
    }else{
        x=std::fabs(x);
        if(ix<0x3f980000){
            if(ix<0x3f300000){id=0;x=dvd(sub(mul(2.0F,x),1.0F),add(2.0F,x));}
            else{id=1;x=dvd(sub(x,1.0F),add(x,1.0F));}
        }else{
            if(ix<0x401c0000){id=2;x=dvd(sub(x,1.5F),add(1.0F,mul(1.5F,x)));}
            else{id=3;x=dvd(-1.0F,x);}
        }
    }
    float z=mul(x,x),w=mul(z,z);
    const float s1=mul(z,add(aT[0],mul(w,add(aT[2],mul(w,add(aT[4],mul(w,add(aT[6],mul(w,add(aT[8],mul(w,aT[10])))))))))));
    const float s2=mul(w,add(aT[1],mul(w,add(aT[3],mul(w,add(aT[5],mul(w,add(aT[7],mul(w,aT[9])))))))));
    if(id<0)return sub(x,mul(x,add(s1,s2)));
    z=sub(atanhi[id],sub(sub(mul(x,add(s1,s2)),atanlo[id]),x));
    return hx<0?-z:z;
}
// atan2f 0011d7a0 (reached through the wrapper 00121ab8).
float atan2f_impl(float y,float x){
    const float pi=fb(0x40490fdaU),pi_o_2=fb(0x3fc90fdaU),pi_lo=fb(0x34222168U);
    const std::int32_t hx=static_cast<std::int32_t>(bf(x)),ix=hx&0x7fffffff,hy=static_cast<std::int32_t>(bf(y)),iy=hy&0x7fffffff;
    if(ix>=0x7f800000||iy>=0x7f800000)throw std::runtime_error("atan2f of a nonfinite value");
    if(hx==0x3f800000)return atanf_impl(y);
    const int m=((hy>>31)&1)|((hx>>30)&2);
    if(iy<=0x7fffff){
        if(m==2)return pi;
        if(m==3)return -pi;
        return y;
    }
    if(ix<=0x7fffff)return hy<0?-pi_o_2:pi_o_2;
    const std::int32_t k=(iy-ix)>>23;float z;
    if(k>=0x3d)z=pi_o_2;
    else if(hx<0&&k< -0x3c)z=0.0F;
    else z=atanf_impl(std::fabs(dvd(y,x)));
    switch(m){
    case 0:return z;
    case 1:return fb(bf(z)^0x80000000U);
    case 2:return sub(pi,sub(z,pi_lo));
    default:return sub(sub(z,pi_lo),pi);
    }
}
// __kernel_tanf 001208f0 (T table 00499b68; pio4lo 0x33222167 as compiled at
// 001209c0: the executable's constants differ from the C source in the last
// bit for T[3], T[4], T[6], T[7], T[8], T[11] and pio4lo).
float kernel_tanf(float x,float y,int iy){
    static const float T[]={fb(0x3eaaaaabU),fb(0x3e088889U),fb(0x3d5d0dd1U),fb(0x3cb327a3U),fb(0x3c11371eU),fb(0x3b6b6916U),
        fb(0x3abede47U),fb(0x3a1a26c7U),fb(0x398137b8U),fb(0x38a3f445U),fb(0x3895c07aU),fb(0xb79bae5eU),fb(0x37d95384U)};
    const float pio4=fb(0x3f490fdaU),pio4lo=fb(0x33222167U);
    const std::int32_t hx=static_cast<std::int32_t>(bf(x)),ix=hx&0x7fffffff;
    if(ix<0x31800000){
        if(static_cast<int>(x)==0){
            if((ix|(iy+1))==0)return dvd(1.0F,std::fabs(x));
            return iy==1?x:dvd(-1.0F,x);
        }
    }
    if(ix>=0x3f2ca140){
        if(hx<0){x=-x;y=-y;}
        const float z0=sub(pio4,x),w0=sub(pio4lo,y);
        x=add(z0,w0);y=0.0F;
    }
    float z=mul(x,x),w=mul(z,z);
    float r=add(T[1],mul(w,add(T[3],mul(w,add(T[5],mul(w,add(T[7],mul(w,add(T[9],mul(w,T[11]))))))))));
    float v=mul(z,add(T[2],mul(w,add(T[4],mul(w,add(T[6],mul(w,add(T[8],mul(w,add(T[10],mul(w,T[12])))))))))));
    float s=mul(z,x);
    r=add(y,mul(z,add(mul(s,add(r,v)),y)));
    r=add(r,mul(T[0],s));
    w=add(x,r);
    if(ix>=0x3f2ca140){
        v=static_cast<float>(iy);
        return mul(static_cast<float>(1-((hx>>31)&2)),sub(v,mul(2.0F,sub(x,sub(dvd(mul(w,w),add(w,v)),r)))));
    }
    if(iy==1)return w;
    z=fb(bf(w)&0xfffff000U);
    v=sub(r,sub(z,x));
    const float a=dvd(-1.0F,w);
    const float t=fb(bf(a)&0xfffff000U);
    s=add(1.0F,mul(t,z));
    return add(t,mul(a,add(s,mul(t,v))));
}
// 0011e950 (fdlibm __ieee754_rem_pio2f structure). pi/4 < |x| < 3pi/4: n =
// +-1 with pio2_1t 0x37354442 and pio2_2 0x373543ff as compiled at
// 0011e9dc/0011e9f0. 3pi/4 <= |x| <= 0x43490f80 (2^7 pi/2): the medium path
// 0011ea98..0011ec24: n = (int)(|x| * 2/pi + 1/2) (cvt.w.s truncates), r =
// |x| - n pio2_1, w = n pio2_1t; y0 = r - w; for n >= 32 or |x| on the
// npio2_hw table entry (00499914 + 4n) a second (pio2_2 / pio2_2t) and third
// (pio2_3 0x2e85a300 / pio2_3t 0x248d3131) step when the exponent dropped by
// more than 8 / 25; y1 = (r - y0) - w; negative x negates y0, y1 and n.
// Larger arguments take the general reduction (0011ec28 -> 0011ff80 below).
//
// scalbnf 00121860: zero/denormal x returns x; an exponent that leaves the
// normal range underflows to +-0 (tiny * tiny under flush-to-zero) or
// overflows (huge * huge, never reached by the reduction: rejected).
float game_scalbnf(float x,int n){
    const std::uint32_t hx=bf(x),ix=hx&0x7fffffffU;
    if(ix<=0x7fffffU)return x;
    const int k=static_cast<int>(ix>>23)+n;
    if(k>=0xff)throw std::runtime_error("scalbnf 00121860 overflow");
    if(k<=0)return (hx&0x80000000U)?-0.0F:0.0F;
    return fb((hx&0x807fffffU)|(static_cast<std::uint32_t>(k)<<23));
}
// __kernel_rem_pio2f 0011ff80 (fdlibm k_rem_pio2f.c, 8-bit chunks): init_jk
// 00499b08 {4,7,9}, PIo2 00499b18, two_over_pi 00499600 (198 bytes as ints),
// floorf 00121788 (exact; std::floor). Only prec 2 (jk = jp = 9) is reached.
int kernel_rem_pio2f(const float* x,float* y,int e0,int nx){
    static constexpr std::uint8_t ipio2[198]={162,249,131,110,78,68,21,41,252,39,87,209,245,52,221,192,219,98,149,153,60,67,144,65,
        254,81,99,171,222,187,197,97,183,36,110,58,66,77,210,224,6,73,46,234,9,209,146,28,254,29,235,28,177,41,167,62,232,130,53,245,
        46,187,68,132,233,156,112,38,180,95,126,65,57,145,214,57,131,83,57,244,156,132,95,139,189,249,40,59,31,248,151,255,222,5,152,
        15,239,47,17,139,90,10,109,31,109,54,126,207,39,203,9,183,79,70,63,102,158,95,234,45,117,39,186,199,235,229,241,123,61,7,57,
        247,138,82,146,234,107,251,95,177,31,141,93,8,86,3,48,70,252,123,107,171,240,207,188,32,154,244,54,29,169,227,145,97,94,230,
        27,8,101,153,133,95,20,160,104,64,141,255,216,128,77,115,39,49,6,6,21,86,202,115,168,201,96,226,123,192,140,107};
    static const float PIo2[]={fb(0x3fc90000U),fb(0x39f00000U),fb(0x37d9ffffU),fb(0x33a20000U),fb(0x2e83ffffU),fb(0x2b4fffffU),
        fb(0x27c1ffffU),fb(0x22cfffffU),fb(0x1fc40000U),fb(0x1bc5ffffU),fb(0x17440000U)};
    const float two8=256.0F,twon8=fb(0x3b800000U);
    const int jk=9,jp=9,jx=nx-1;
    int iq[20]{},jv=(e0-3)/8;
    float f[20]{},q[20]{},fq[20]{},z,fw;
    if(jv<0)jv=0;
    int q0=e0-8*(jv+1);
    auto chunk=[&](int j){
        if(j>=198)throw std::runtime_error("__kernel_rem_pio2f 0011ff80: two_over_pi index beyond 00499600 table");
        return j<0?0.0F:static_cast<float>(ipio2[j]);};
    for(int i=0,j=jv-jx;i<=jx+jk;++i,++j)f[i]=chunk(j);
    for(int i=0;i<=jk;++i){fw=0;for(int j=0;j<=jx;++j)fw=add(fw,mul(x[j],f[jx+i-j]));q[i]=fw;}
    int jz=jk,n,ih;
    for(;;){
        z=q[jz];
        for(int i=0,j=jz;j>0;++i,--j){
            fw=static_cast<float>(static_cast<std::int32_t>(mul(twon8,z)));
            iq[i]=static_cast<std::int32_t>(sub(z,mul(two8,fw)));
            z=add(q[j-1],fw);
        }
        z=game_scalbnf(z,q0);
        z=sub(z,mul(8.0F,std::floor(mul(z,0.125F))));
        n=static_cast<std::int32_t>(z);
        z=sub(z,static_cast<float>(n));
        ih=0;
        if(q0>0){
            const int i=iq[jz-1]>>(8-q0);n+=i;
            iq[jz-1]-=i<<(8-q0);
            ih=iq[jz-1]>>(7-q0);
        }else if(q0==0)ih=iq[jz-1]>>8;                                  // 001201b8: sra 8 as compiled (not 7)
        else if(z>=0.5F)ih=2;
        if(ih>0){
            n+=1;int carry=0;
            for(int i=0;i<jz;++i){
                const int j=iq[i];
                if(carry==0){if(j!=0){carry=1;iq[i]=0x100-j;}}
                else iq[i]=0xff-j;
            }
            if(q0==1)iq[jz-1]&=0x7f;
            else if(q0==2)iq[jz-1]&=0x3f;
            if(ih==2){z=sub(1.0F,z);if(carry!=0)z=sub(z,game_scalbnf(1.0F,q0));}
        }
        if(z==0){
            int j=0;
            for(int i=jz-1;i>=jk;--i)j|=iq[i];
            if(j==0){
                int k=1;
                while(iq[jk-k]==0)++k;
                for(int i=jz+1;i<=jz+k;++i){
                    if(jx+i>=20)throw std::runtime_error("__kernel_rem_pio2f 0011ff80: recompute beyond its 20-entry frame");
                    f[jx+i]=chunk(jv+i);
                    fw=0;for(int jj=0;jj<=jx;++jj)fw=add(fw,mul(x[jj],f[jx+i-jj]));q[i]=fw;
                }
                jz+=k;
                continue;
            }
        }
        break;
    }
    if(z==0){
        jz-=1;q0-=8;
        while(iq[jz]==0){--jz;q0-=8;}
    }else{
        z=game_scalbnf(z,-q0);
        if(z>=two8){
            fw=static_cast<float>(static_cast<std::int32_t>(mul(twon8,z)));
            iq[jz]=static_cast<std::int32_t>(sub(z,mul(two8,fw)));
            jz+=1;q0+=8;
            iq[jz]=static_cast<std::int32_t>(fw);
        }else iq[jz]=static_cast<std::int32_t>(z);
    }
    fw=game_scalbnf(1.0F,q0);
    for(int i=jz;i>=0;--i){q[i]=mul(fw,static_cast<float>(iq[i]));fw=mul(fw,twon8);}
    for(int i=jz;i>=0;--i){
        fw=0;
        for(int k=0;k<=jp&&k<=jz-i;++k)fw=add(fw,mul(PIo2[k],q[i+k]));
        fq[jz-i]=fw;
    }
    fw=0;
    for(int i=jz;i>=0;--i)fw=add(fw,fq[i]);
    y[0]=ih==0?fw:-fw;
    fw=sub(fq[0],fw);
    for(int i=1;i<=jz;++i)fw=add(fw,fq[i]);
    y[1]=ih==0?fw:-fw;
    return n&7;
}
// 0011ec28..0011ecc4: |x| > 2^7 pi/2. e0 = ilogb(|x|) - 7; z = |x| scaled to [2^7, 2^8);
// three 8-bit pieces tx[0..2] (trailing zero pieces dropped).
int reduce_general(std::int32_t hx,float& y0,float& y1){
    const std::int32_t ix=hx&0x7fffffff;
    const int e0=(ix>>23)-134;
    float z=fb(static_cast<std::uint32_t>(ix-(e0<<23))),tx[3],y[2];
    for(int i=0;i<2;++i){
        tx[i]=static_cast<float>(static_cast<std::int32_t>(z));
        z=mul(sub(z,tx[i]),256.0F);
    }
    tx[2]=z;
    int nx=3;
    while(tx[nx-1]==0)--nx;
    const int n=kernel_rem_pio2f(tx,y,e0,nx);
    if(hx<0){y0=-y[0];y1=-y[1];return -n;}
    y0=y[0];y1=y[1];
    return n;
}
int reduce_medium(float x,float& y0,float& y1,const char* caller){
    const std::int32_t hx=static_cast<std::int32_t>(bf(x)),ix=hx&0x7fffffff;
    if(ix>=0x7f800000)throw std::runtime_error(std::string(caller)+" of a nonfinite value");
    if(ix>0x43490f80)return reduce_general(hx,y0,y1);
    const float pio2_1=fb(0x3fc90f80U),pio2_1t=fb(0x37354442U),pio2_2=fb(0x373543ffU),pio2_2t=fb(0x2e85a308U);
    if(ix<=0x3f490fd8){y0=x;y1=0;return 0;}
    if(ix<0x4016cbe4){
        if(hx>0){
            float z=sub(x,pio2_1);
            if((ix&0xfffffff0)!=0x3fc90fd0){y0=sub(z,pio2_1t);y1=sub(sub(z,y0),pio2_1t);}
            else{z=sub(z,pio2_2);y0=sub(z,pio2_2t);y1=sub(sub(z,y0),pio2_2t);}
            return 1;
        }
        float z=add(x,pio2_1);
        if((ix&0xfffffff0)!=0x3fc90fd0){y0=add(z,pio2_1t);y1=add(sub(z,y0),pio2_1t);}
        else{z=add(z,pio2_2);y0=add(z,pio2_2t);y1=add(sub(z,y0),pio2_2t);}
        return -1;
    }
    // npio2_hw (00499918, entry n-1 read at 00499914 + 4n).
    static constexpr std::uint32_t npio2_hw[32]={0x3fc90f00,0x40490f00,0x4096cb00,0x40c90f00,0x40fb5300,0x4116cb00,0x412fed00,0x41490f00,
        0x41623100,0x417b5300,0x418a3a00,0x4196cb00,0x41a35c00,0x41afed00,0x41bc7e00,0x41c90f00,0x41d5a000,0x41e23100,0x41eec200,
        0x41fb5300,0x4203f200,0x420a3a00,0x42108300,0x4216cb00,0x421d1400,0x42235c00,0x4229a500,0x422fed00,0x42363600,0x423c7e00,0x4242c700,0x42490f00};
    const float t=fb(static_cast<std::uint32_t>(ix));                         // 00121760 fabsf
    const int n=static_cast<int>(add(mul(t,fb(0x3f22f984U)),0.5F));          // invpio2, cvt.w.s
    const float fn=static_cast<float>(n);
    float r=sub(t,mul(fn,pio2_1)),w=mul(fn,pio2_1t);
    y0=sub(r,w);
    if(!(n<32&&static_cast<std::uint32_t>(ix&static_cast<std::int32_t>(0xffffff00))!=npio2_hw[n-1])){
        const int j=ix>>23;
        if(j-static_cast<int>((bf(y0)>>23)&0xffU)>=9){
            float u=r;w=mul(fn,pio2_2);r=sub(u,w);w=sub(mul(fn,pio2_2t),sub(sub(u,r),w));y0=sub(r,w);
            if(j-static_cast<int>((bf(y0)>>23)&0xffU)>=26){
                u=r;w=mul(fn,fb(0x2e85a300U));r=sub(u,w);w=sub(mul(fn,fb(0x248d3131U)),sub(sub(u,r),w));y0=sub(r,w);
            }
        }
    }
    y1=sub(sub(r,y0),w);
    if(hx<0){y0=-y0;y1=-y1;return -n;}
    return n;
}
// tanf 00121a10.
float tanf_impl(float x){
    const std::int32_t ix=static_cast<std::int32_t>(bf(x))&0x7fffffff;
    if(ix<=0x3f490fda)return kernel_tanf(x,0.0F,1);
    float y0,y1;
    const int n=reduce_medium(x,y0,y1,"tanf");
    return kernel_tanf(y0,y1,1-((n&1)<<1));
}
// __kernel_sinf 001207e8 (S1..S6 inline: 0xbe2aaaaa, 0x3c088889, 0xb9500d01,
// 0x3638ef1a, 0xb2d72f34, 0x2f2ec9d2).
float kernel_sinf(float x,float y,int iy){
    const std::int32_t ix=static_cast<std::int32_t>(bf(x))&0x7fffffff;
    if(ix<0x32000000&&static_cast<int>(x)==0)return x;           // cvt.w.s truncates
    const float z=mul(x,x),v=mul(z,x);
    const float r=add(mul(z,add(mul(z,add(mul(z,add(mul(z,fb(0x2f2ec9d2U)),fb(0xb2d72f34U))),fb(0x3638ef1aU))),fb(0xb9500d01U))),fb(0x3c088889U));
    if(iy==0)return add(x,mul(v,add(mul(z,r),fb(0xbe2aaaaaU))));
    return sub(x,sub(sub(mul(z,sub(mul(y,0.5F),mul(v,r))),y),mul(v,fb(0xbe2aaaaaU))));
}
// __kernel_cosf 0011fe30 (C1..C6 inline: 0x3d2aaaaa, 0xbab60b60, 0x37d00d00,
// 0xb493f27b, 0x310f74f5, 0xad47d74e).
float kernel_cosf(float x,float y){
    const std::int32_t ix=static_cast<std::int32_t>(bf(x))&0x7fffffff;
    if(ix<0x32000000&&static_cast<int>(x)==0)return 1.0F;
    const float z=mul(x,x);
    const float poly=add(mul(z,add(mul(z,add(mul(z,add(mul(z,add(mul(z,fb(0xad47d74eU)),fb(0x310f74f5U))),fb(0xb493f27bU))),fb(0x37d00d00U))),fb(0xbab60b60U))),fb(0x3d2aaaaaU));
    const float r=mul(z,poly);
    if(ix<0x3e99999a)return sub(1.0F,sub(mul(z,0.5F),sub(mul(z,r),mul(x,y))));
    const float qx=ix>0x3f480000?fb(0x3e900000U):fb(static_cast<std::uint32_t>(ix-0x01000000));
    const float hz=sub(mul(z,0.5F),qx),a=sub(1.0F,qx);
    return sub(a,sub(hz,sub(mul(z,r),mul(x,y))));
}
// sinf 00121928 / cosf 00121688.
float sinf_impl(float x){
    const std::int32_t ix=static_cast<std::int32_t>(bf(x))&0x7fffffff;
    if(ix<=0x3f490fd8)return kernel_sinf(x,0.0F,0);
    float y0,y1;
    switch(reduce_medium(x,y0,y1,"sinf")&3){
    case 0:return kernel_sinf(y0,y1,1);
    case 1:return kernel_cosf(y0,y1);
    case 2:return -kernel_sinf(y0,y1,1);
    default:return -kernel_cosf(y0,y1);
    }
}
float cosf_impl(float x){
    const std::int32_t ix=static_cast<std::int32_t>(bf(x))&0x7fffffff;
    if(ix<=0x3f490fd8)return kernel_cosf(x,0.0F);
    float y0,y1;
    switch(reduce_medium(x,y0,y1,"cosf")&3){
    case 0:return kernel_cosf(y0,y1);
    case 1:return -kernel_sinf(y0,y1,1);
    case 2:return -kernel_cosf(y0,y1);
    default:return kernel_sinf(y0,y1,1);
    }
}
// acosf 00121a88 -> 0011cfd8 (fdlibm e_acosf; sqrtf 0011ed00 is the software
// e_sqrtf, correctly rounded like the host's). |x| > 1 and NaN (the kernel's
// (x - x) / (x - x)) throw.
float acosf_impl(float x){
    const float pi=fb(0x40490fdaU),pio2_hi=fb(0x3fc90fdaU),pio2_lo=fb(0x33a22168U);
    const float pS0=fb(0x3e2aaaaaU),pS1=fb(0xbea6b08fU),pS2=fb(0x3e4e0aa8U),pS3=fb(0xbd241146U),pS4=fb(0x3a4f7f04U),pS5=fb(0x3811ef08U);
    const float qS1=fb(0xc019d138U),qS2=fb(0x4001572cU),qS3=fb(0xbf303360U),qS4=fb(0x3d9dc62dU);
    const std::int32_t hx=static_cast<std::int32_t>(bf(x)),ix=hx&0x7fffffff;
    if(ix==0x3f800000)return hx>0?0.0F:pi;
    if(ix>0x3f800000)throw std::runtime_error("acosf outside [-1, 1]");
    const auto pq=[&](float z){
        const float p=mul(z,add(mul(z,add(mul(z,add(mul(z,add(mul(z,add(mul(z,pS5),pS4)),pS3)),pS2)),pS1)),pS0));
        const float q=add(mul(z,add(mul(z,add(mul(z,add(mul(z,qS4),qS3)),qS2)),qS1)),1.0F);
        return dvd(p,q);
    };
    if(ix<0x3f000000){
        if(ix<=0x23000000)return pio2_hi;
        const float r=pq(mul(x,x));
        return sub(pio2_hi,sub(x,sub(pio2_lo,mul(x,r))));
    }
    if(hx<0){
        const float z=mul(add(x,1.0F),0.5F),s=std::sqrt(z),r=pq(z);
        const float w=add(s,sub(mul(r,s),pio2_lo));
        return sub(pi,add(w,w));
    }
    const float z=mul(sub(1.0F,x),0.5F),s=std::sqrt(z);
    const float df=fb(bf(s)&0xfffff000U);
    const float c=dvd(sub(z,mul(df,df)),add(s,df));
    const float r=pq(z);
    const float w=add(df,add(mul(r,s),c));
    return add(w,w);
}
// Checkpoint 132cp9: asinf 00121aa0 -> 0011d400 (fdlibm e_asinf, the acosf
// pS / qS constants; fabsf 00121760, sqrtf 0011ed00). |x| > 1 (the kernel's
// (x - x) / (x - x)) throws.
float asinf_impl(float x){
    const float pio2_hi=fb(0x3fc90fdaU),pio2_lo=fb(0x33a22168U),pio4_hi=fb(0x3f490fdaU);
    const float pS0=fb(0x3e2aaaaaU),pS1=fb(0xbea6b08fU),pS2=fb(0x3e4e0aa8U),pS3=fb(0xbd241146U),pS4=fb(0x3a4f7f04U),pS5=fb(0x3811ef08U);
    const float qS1=fb(0xc019d138U),qS2=fb(0x4001572cU),qS3=fb(0xbf303360U),qS4=fb(0x3d9dc62dU);
    const std::int32_t hx=static_cast<std::int32_t>(bf(x)),ix=hx&0x7fffffff;
    if(ix==0x3f800000)return add(mul(x,pio2_hi),mul(x,pio2_lo));
    if(ix>0x3f800000)throw std::runtime_error("asinf outside [-1, 1] (0011d46c)");
    const auto p_of=[&](float t){return mul(t,add(mul(t,add(mul(t,add(mul(t,add(mul(t,add(mul(t,pS5),pS4)),pS3)),pS2)),pS1)),pS0));};
    const auto q_of=[&](float t){return add(mul(t,add(mul(t,add(mul(t,add(mul(t,qS4),qS3)),qS2)),qS1)),1.0F);};
    if(ix<0x3f000000){
        if(ix<=0x31ffffff)return x;   // huge + x > 1 always holds
        const float t=mul(x,x);
        return add(x,mul(x,dvd(p_of(t),q_of(t))));
    }
    const float t=mul(sub(1.0F,fb(static_cast<std::uint32_t>(ix))),0.5F);
    const float p=p_of(t),q=q_of(t),s=std::sqrt(t);
    float r;
    if(ix>=0x3f79999a){
        const float w=dvd(p,q);
        const float u=add(s,mul(s,w));
        r=sub(pio2_hi,sub(add(u,u),pio2_lo));
    }else{
        const float df=fb(bf(s)&0xfffff000U);
        const float c=dvd(sub(t,mul(df,df)),add(s,df));
        const float w=dvd(p,q);
        const float a=sub(mul(add(s,s),w),sub(pio2_lo,add(c,c)));
        const float b=sub(pio4_hi,add(df,df));
        r=sub(pio4_hi,sub(a,b));
    }
    return hx>0?r:fb(bf(r)^0x80000000U);
}
// powf 00121b30 -> 0011e170 (fdlibm e_powf as compiled: no subnormal or
// infinity cases, |y| > 2^27 folded to FLT_MAX (00499 5fc) or 0, t_h built
// as ((ix >> 1) | 0x20000000) + 0x40000 + (k << 21), constants as the
// executable's lui/ori words). The (x - x) / (x - x) domain result is
// 0 / 0 = +FLT_MAX under div.s. Overflow (huge * huge, which the interpreter
// leaves infinite and the EE clamps) throws, as does the exponent-255 result
// the 129 overflow threshold (0x43010000 at 0011e6e8) lets through.
float powf_impl(float x,float y){
    const auto mask=[](float v){return fb(bf(v)&0xfffff000U);};
    const std::int32_t hx=static_cast<std::int32_t>(bf(x)),hy=static_cast<std::int32_t>(bf(y));
    const std::int32_t iy=hy&0x7fffffff;std::int32_t ix=hx&0x7fffffff;
    if(iy<=0x7fffff)return 1.0F;                                                // 0011e1c4
    int yisint=0;
    if(hx<0){
        if(iy>0x4b7fffff)yisint=2;
        else if(iy>0x3f7fffff){
            const int sh=0x96-(iy>>23);
            const std::int32_t j=iy>>sh;
            if((j<<sh)==iy)yisint=2-(j&1);
        }
    }
    if(iy==0x3f800000)return hy>=0?x:dvd(1.0F,x);                              // 0011e22c
    if(hy==0x40000000){
        const float r=mul(x,x);
        if(std::isinf(r))throw std::runtime_error("powf 0011e170 overflow (x * x at 0011e264)");
        return r;
    }
    if(hy==0x3f000000&&hx>=0)return ix==0?x:std::sqrt(x);                      // sqrtf 0011ed00
    const float ax=fb(static_cast<std::uint32_t>(ix));                          // fabsf 00121760
    const int sx=static_cast<int>(static_cast<std::uint32_t>(hx)>>31);
    if(ix<=0x7fffff||ix==0x3f800000){                                           // 0011e2c8
        float z=ax;
        if(hy<0)z=dvd(1.0F,z);
        if(hx<0){
            if(((ix-0x3f800000)|yisint)==0){const float d=sub(z,z);z=dvd(d,d);}
            else if(yisint==1)z=-z;
        }
        return z;
    }
    if(((sx-1)|yisint)==0){const float d=sub(x,x);return dvd(d,d);}            // 0011e338
    float t1,t2;
    if(iy>0x4d000000){                                                          // 0011e350
        const float big=fb(0x7f7fffffU);
        if(ix<=0x3f7ffff7)return hy<0?big:0.0F;
        if(ix>0x3f800007)return hy>0?big:0.0F;
        const float t=sub(x,1.0F);                                              // x, not |x| (0011e3c0)
        const float w=mul(mul(t,t),sub(0.5F,mul(t,sub(fb(0x3eaaaaaaU),mul(t,0.25F)))));
        const float u=mul(t,fb(0x3fb8aa00U));
        const float v=sub(mul(t,fb(0x36eca56fU)),mul(w,fb(0x3fb8aa3aU)));
        t1=mask(add(u,v));
        t2=sub(v,sub(t1,u));
    }else{                                                                      // 0011e448
        static const float bp[]={1.0F,1.5F},dp_h[]={0.0F,fb(0x3f15c000U)},dp_l[]={0.0F,fb(0x35d1cfdcU)}; // 00499578/80/88
        const std::int32_t j=ix&0x7fffff;
        int n=(ix>>23)-0x7f;
        ix=j|0x3f800000;
        int k=0;
        if(j>0x1cc471){
            if(j<=0x5db3d6)k=1;
            else{n=(static_cast<std::int32_t>(bf(x)&0x7fffffffU)>>23)-0x7e;ix-=0x800000;}
        }
        const float a=fb(static_cast<std::uint32_t>(ix));
        const float u=sub(a,bp[k]),v=dvd(1.0F,add(a,bp[k]));
        const float s=mul(u,v),s_h=mask(s);
        float t_h=fb(static_cast<std::uint32_t>(((ix>>1)|0x20000000)+0x40000+(k<<21)));
        const float t_l=sub(a,sub(t_h,bp[k]));
        const float s_l=mul(v,sub(sub(u,mul(s_h,t_h)),mul(s_h,t_l)));
        const float s2=mul(s,s);
        float r=add(mul(s2,fb(0x3e53f142U)),fb(0x3e6c3254U));
        r=add(mul(s2,r),fb(0x3e8ba304U));
        r=add(mul(s2,r),fb(0x3eaaaaabU));
        r=add(mul(s2,r),fb(0x3edb6db7U));
        r=add(mul(s2,r),fb(0x3f199999U));
        r=add(mul(mul(s2,s2),r),mul(s_l,add(s_h,s)));
        const float sh2=mul(s_h,s_h);
        t_h=mask(add(add(sh2,3.0F),r));
        const float tl2=sub(r,sub(sub(t_h,3.0F),sh2));
        const float uu=mul(s_h,t_h),vv=add(mul(s_l,t_h),mul(tl2,s));
        const float p_h=mask(add(uu,vv)),p_l=sub(vv,sub(p_h,uu));
        const float z_h=mul(p_h,fb(0x3f763800U));
        const float z_l=add(add(mul(p_h,fb(0x369dc39fU)),mul(p_l,fb(0x3f76384eU))),dp_l[k]);
        const float t=static_cast<float>(n);
        t1=mask(add(add(add(z_h,z_l),dp_h[k]),t));
        t2=sub(z_l,sub(sub(sub(t1,t),dp_h[k]),z_h));
    }
    const float sn=((sx-1)|(yisint-1))==0?-1.0F:1.0F;                          // 0011e67c
    const float y1=mask(y);
    float p_l=add(mul(sub(y,y1),t1),mul(y,t2));
    float p_h=mul(y1,t1);
    const float z=add(p_l,p_h);
    const std::int32_t jz=static_cast<std::int32_t>(bf(z)),i=jz&0x7fffffff;
    if(jz>0){
        if(i>0x43010000||(i==0x43010000&&sub(z,p_h)<add(p_l,fb(0x3338aa3bU))))
            throw std::runtime_error("powf 0011e170 overflow (sn * huge * huge at 0011e6f8)");
    }else if(i>0x42fc0000||(i==0x42fc0000&&p_l<=sub(z,p_h))){
        const float tiny=fb(0x0da2425fU);
        return mul(mul(sn,tiny),tiny);
    }
    int n=0;std::uint32_t zw=bf(z);
    if(i>0x3f000000){                                                           // 0011e7a0
        const std::int32_t m=jz+(0x800000>>((i>>23)-0x7e));
        const int kk=static_cast<int>((static_cast<std::uint32_t>(m)>>23)&0xff)-0x7f;
        const float t=fb(static_cast<std::uint32_t>(m)&~(0x7fffffU>>kk));
        n=((m&0x7fffff)|0x800000)>>(23-kk);
        if(jz<0)n=-n;
        p_h=sub(p_h,t);
        zw=bf(add(p_l,p_h));
    }
    const float t=fb(zw&0xfffff000U);                                           // 0011e800
    const float u=mul(t,fb(0x3f317200U));
    const float v=add(mul(sub(p_l,sub(t,p_h)),fb(0x3f317217U)),mul(t,fb(0x35bfbe8cU)));
    float zz=add(u,v);
    const float w=sub(v,sub(zz,u));
    const float tt=mul(zz,zz);
    float c=add(mul(tt,fb(0x3331bb4bU)),fb(0xb5ddea0eU));
    c=add(mul(tt,c),fb(0x388ab354U));
    c=add(mul(tt,c),fb(0xbb360b60U));
    c=add(mul(tt,c),fb(0x3e2aaaaaU));
    const float tc=sub(zz,mul(tt,c));
    const float rr=sub(dvd(mul(zz,tc),sub(tc,2.0F)),add(w,mul(zz,w)));
    zz=sub(1.0F,sub(rr,zz));
    const std::int32_t jj=static_cast<std::int32_t>(bf(zz))+static_cast<std::int32_t>(static_cast<std::uint32_t>(n)<<23);
    // The overflow test above is against 0x43010000 (129), so z in [128, 129]
    // lands on exponent 255 here: an infinity/NaN pattern to the interpreter,
    // a finite operand the EE then clamps. Rejected like huge * huge.
    if((jj>>23)>=0xff)throw std::runtime_error("powf 0011e170 overflow (exponent 255 at 0011e900)");
    zz=(jj>>23)>0?fb(static_cast<std::uint32_t>(jj)):game_scalbnf(zz,n);       // scalbnf 00121860
    return mul(sn,zz);
}
}
float game_acosf(float x){return acosf_impl(x);}
float game_asinf(float x){return asinf_impl(x);}
float game_atan2f(float y,float x){return atan2f_impl(y,x);}
float game_atanf(float x){if(!std::isfinite(x))throw std::runtime_error("atanf of a nonfinite value");return atanf_impl(x);}
float game_tanf(float x){return tanf_impl(x);}
float game_sinf(float x){if(!std::isfinite(x))throw std::runtime_error("sinf of a nonfinite value");return sinf_impl(x);}
float game_cosf(float x){if(!std::isfinite(x))throw std::runtime_error("cosf of a nonfinite value");return cosf_impl(x);}
float game_powf(float x,float y){
    if(!std::isfinite(x)||!std::isfinite(y))throw std::runtime_error("powf 0011e170 of a nonfinite value");
    if(std::fpclassify(x)==FP_SUBNORMAL)throw std::runtime_error("powf 0011e170 of a subnormal x (EE and interpreter disagree)");
    return powf_impl(x,y);
}
}
