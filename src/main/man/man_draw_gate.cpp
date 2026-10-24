#include "man_draw_gate.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& message){throw std::runtime_error(message);}

std::uint32_t bits(float f){return std::bit_cast<std::uint32_t>(f);}
float real_of(std::uint32_t b){return std::bit_cast<float>(b);}
bool exponent_max(float f){return (bits(f)&0x7f800000U)==0x7f800000U;}
bool denormal(float f){return (bits(f)&0x7f800000U)==0&&(bits(f)&0x007fffffU)!=0;}
float flush(float f){return denormal(f)?real_of(bits(f)&0x80000000U):f;}

// VU0 micro-mode upper arithmetic on VU float words, as the interpreter runs it with
// vu_float_range (tools/ee/ee_vu0.py; checkpoint 132uk5 for this routine): a zero exponent
// reads as signed zero, exponent 255 is the normal binade 1.F x 2^128 (the VU has no
// infinity or NaN, VU User's Manual 2.1.1); binary32 round to nearest, a result past
// exponent 255 clamps to +-MAX (0x7fffffff) and one below the normal range flushes to
// signed zero (Manual 2.3). The operands are binary32 values below 2^129, so one double
// operation then this rounding is the correctly rounded binary32 operation.
constexpr const char* cull="003c8990";
using Word=std::uint32_t;
using Lanes=std::array<Word,4>;
constexpr Word kVuMax=0x7fffffffU;
double vval(Word b){
    const Word ex=(b>>23)&0xffU;
    if(ex==0)return (b>>31)?-0.0:0.0;
    if(ex==0xff){
        const double m=std::ldexp(1.0+static_cast<double>(b&0x7fffffU)/8388608.0,128);
        return (b>>31)?-m:m;
    }
    return static_cast<double>(std::bit_cast<float>(b));
}
Word vrnd(double x){
    if(std::isnan(x))fail(std::string(cull)+": VU0 NaN result (not ported)");
    const Word sign=std::signbit(x)?0x80000000U:0U;
    const double a=std::fabs(x);
    const Word b=std::bit_cast<Word>(static_cast<float>(a));
    if(b!=0x7f800000U)return sign|(((b>>23)&0xffU)==0?0U:b);
    const Word h=std::bit_cast<Word>(static_cast<float>(a*0.5));   // the 2^128 binade at half scale
    if(h==0x7f800000U)return sign|kVuMax;
    return sign|(h+0x800000U);
}
Word vmul_exact(Word a,Word b){return vrnd(vval(a)*vval(b));}
Word vadd_exact(Word a,Word b){return vrnd(vval(a)+vval(b));}
Word vsub_exact(Word a,Word b){return vrnd(vval(a)-vval(b));}
// Port-only fast path (132vt2d, exact): with no operand at exponent 255, one binary32 operation is the same correctly
// rounded result as the double operation then vrnd (a binary32 product is exact in double; a sum's double rounding is
// innocuous, 53 >= 2 x 24 + 2), with a zero-exponent operand read as its signed zero. A result at exponent 2..254 is
// that word; one at exponent 0 (zero, or tiny: vrnd flushes it) is its signed zero; exponent 1 (a tiny exact result
// rounded up, or flushed by a flush-to-zero unit) and 255 (overflow) take the double path.
bool vfast(Word w){return ((w>>23)&0xffU)!=0xffU;}
float vfloat(Word w){return std::bit_cast<float>(((w>>23)&0xffU)==0?(w&0x80000000U):w);}
bool vdone(float r,Word& out){
    const Word b=std::bit_cast<Word>(r),ex=(b>>23)&0xffU;
    if(ex==0){out=b&0x80000000U;return true;}
    if(ex>=2&&ex<=254){out=b;return true;}
    return false;
}
Word vmul(Word a,Word b){
    Word out;
    if(vfast(a)&&vfast(b)&&vdone(vfloat(a)*vfloat(b),out))return out;
    return vmul_exact(a,b);
}
Word vadd(Word a,Word b){
    Word out;
    if(vfast(a)&&vfast(b)&&vdone(vfloat(a)+vfloat(b),out))return out;
    return vadd_exact(a,b);
}
Word vsub(Word a,Word b){
    Word out;
    if(vfast(a)&&vfast(b)&&vdone(vfloat(a)-vfloat(b),out))return out;
    return vsub_exact(a,b);
}
// MAX orders the words sign-magnitude and transfers the operand unchanged.
std::int64_t vkey(Word b){return (b>>31)?-static_cast<std::int64_t>(b&0x7fffffffU):static_cast<std::int64_t>(b);}
Word vmax(Word a,Word b){
    if(vkey(a)==vkey(b)){
        if(a!=b)fail(std::string(cull)+": VMAX of +0 and -0 (not ported)");
        return a;
    }
    return vkey(a)<vkey(b)?b:a;
}
constexpr Word kOne=0x3f800000U,kMinusOne=0xbf800000U;

// mulax / madday / maddaz / maddw: ((m0*v.x + m1*v.y) + m2*v.z) + m3*v.w per lane.
Lanes combine(const std::array<Lanes,4>& m,const Lanes& v){
    Lanes out{};
    for(std::size_t k=0;k<4;++k){
        Word acc=vmul(m[0][k],v[0]);
        acc=vadd(acc,vmul(m[1][k],v[1]));
        acc=vadd(acc,vmul(m[2][k],v[2]));
        out[k]=vadd(acc,vmul(m[3][k],v[3]));
    }
    return out;
}

// clipw.xyz: x > |w| -> 1, x < -|w| -> 2, y -> 4 / 8, z -> 0x10 / 0x20.
std::uint32_t clip_code(const Lanes& c){
    const double w=std::fabs(vval(c[3]));
    std::uint32_t b=0;
    for(std::size_t a=0;a<3;++a){
        const double x=vval(c[a]);
        if(x>w)b|=1U<<(2*a);
        if(x<-w)b|=2U<<(2*a);
    }
    return b;
}
// EE FPU sub.s / mul.s as the interpreter (tools/ee/ee.py) runs them: binary32 result,
// flushed when denormal. Denormal operands (the hardware flushes them) and exponent-255
// operands or results are not ported.
constexpr const char* gate="003872d8";
float ee_arith(float a,float b,bool multiply){
    if(exponent_max(a)||exponent_max(b)||denormal(a)||denormal(b))
        fail(std::string(gate)+": sub.s / mul.s operand denormal or with exponent 255 (not ported)");
    const float r=flush(multiply?a*b:a-b);
    if(exponent_max(r))fail(std::string(gate)+": sub.s / mul.s overflows (not ported)");
    return r;
}
// cvt.w.s: truncate toward zero, saturating.
std::int32_t cvt_w(float f){
    if(std::isnan(f))fail(std::string(gate)+": cvt.w.s of NaN (not ported)");
    if(f>=2147483647.0F)return 2147483647;
    if(f<=-2147483648.0F)return -2147483647-1;
    return static_cast<std::int32_t>(f);
}
}

ObjectCull object_cull_003c8990(const ShadowMatrix& clip,float near_z,const std::array<float,3>& lo,const std::array<float,3>& hi,
    const std::optional<ShadowMatrix>& transform){
    const auto word=[](float f){return std::bit_cast<Word>(f);};
    const auto lanes=[&](const std::array<float,4>& v){return Lanes{word(v[0]),word(v[1]),word(v[2]),word(v[3])};};
    std::array<Lanes,4> c{};
    for(std::size_t i=0;i<4;++i)c[i]=lanes(clip.column[i]);
    // Micro program 0: vf5..8 = clip qwords; vf1..4 = clip x transform qword i (vi13 =
    // data 5.w set), else vf31 (+0) + clip qword i.
    std::array<Lanes,4> m{};
    for(std::size_t i=0;i<4;++i){
        if(transform)m[i]=combine(c,lanes(transform->column[i]));
        else for(std::size_t k=0;k<4;++k)m[i][k]=vadd(0U,c[i][k]);
    }
    const std::array<Word,3> l{word(lo[0]),word(lo[1]),word(lo[2])},h{word(hi[0]),word(hi[1]),word(hi[2])};
    // Corner 0 (min,min,min) accumulates vf4*1 + vf2*y + vf3*z + vf1*x; the others
    // vf1*x + vf2*y + vf3*z + vf4*1, in the order (max,max,max), (min,min,max),
    // (min,max,min), (min,max,max), (max,min,min), (max,min,max), (max,max,min).
    std::array<Lanes,8> corner{};
    for(std::size_t k=0;k<4;++k){
        Word acc=vmul(m[3][k],kOne);
        acc=vadd(acc,vmul(m[1][k],l[1]));
        acc=vadd(acc,vmul(m[2][k],l[2]));
        corner[0][k]=vadd(acc,vmul(m[0][k],l[0]));
    }
    static constexpr std::array<std::array<bool,3>,7> pick{{{true,true,true},{false,false,true},{false,true,false},{false,true,true},
        {true,false,false},{true,false,true},{true,true,false}}};
    for(std::size_t n=0;n<7;++n){
        const Lanes p{pick[n][0]?h[0]:l[0],pick[n][1]?h[1]:l[1],pick[n][2]?h[2]:l[2],kOne};
        corner[n+1]=combine(m,p);
    }
    std::uint32_t any=0,all=0x3f,behind=0;
    for(const Lanes& n:corner){
        const std::uint32_t b=clip_code(n);
        any|=b;all&=b;
        if(b&0x20U)++behind;
    }
    // vf22 = VMAX over the corners (all lanes), in the order max(c0, c1), c2, ..., c7.
    Lanes top{};
    for(std::size_t k=0;k<4;++k){
        top[k]=vmax(corner[0][k],corner[1][k]);
        for(std::size_t n=2;n<8;++n)top[k]=vmax(top[k],corner[n][k]);
    }
    ObjectCull out;
    out.dist=std::bit_cast<float>(vmul(top[2],kMinusOne));   // muli.z vf23, vf22, I (-1.0); may be an exponent-255 word
    if(all>0){out.code=0x80;return out;}
    // sub.z vf22, vf22, vf9 (near); fmand with 0x20: the z sign flag of the result.
    const std::uint32_t front=(vsub(top[2],word(near_z))>>31)?0U:0x10U;
    out.code=front|(behind>0?4U:(any!=0?2U:1U));
    return out;
}
std::uint32_t object_cull_vu_op(std::uint32_t a,std::uint32_t b,int op,bool fast){
    if(fast)return op==0?vmul(a,b):op==1?vadd(a,b):vsub(a,b);
    return op==0?vmul_exact(a,b):op==1?vadd_exact(a,b):vsub_exact(a,b);
}
std::uint32_t man_draw_flags_0037c350(std::uint32_t mode_word,std::uint32_t code,std::uint32_t a3){
    if(code&0x80U)return 0x80000000U;
    std::uint32_t s=(mode_word|a3)&0xbfffffffU;
    if(!(s&0x80000U))throw std::runtime_error("0037c678: the draw-flags path without 0x80000 is not ported");
    if((s&0x180U)&&!(code&0x10U))s&=~0x180U;
    if(code&4U)s|=0x40000000U;
    return s;
}

ManDrawAlpha man_draw_alpha_003872d8(const ManDrawAlphaInput& in,std::uint16_t h2bec_before){
    ManDrawAlpha out;
    out.h2bec=h2bec_before;out.h2bd8=in.h2bd8;
    auto set_globals=[&](std::uint32_t alpha){out.wrote_globals=true;out.globals={1U,5U,alpha,3U};};
    auto set_2bec=[&](std::uint32_t v){out.h2bec=static_cast<std::uint16_t>(v);out.wrote_2bec=true;};
    auto lh_2bec=[&]{return static_cast<std::int32_t>(static_cast<std::int16_t>(out.h2bec));};
    if(in.a3||in.t0){
        out.s7=0x80U;
        set_globals(0x80U);
        return out;
    }
    if(in.g4933e8>=0){
        set_globals(static_cast<std::uint32_t>(in.g4933e8));
        set_2bec(static_cast<std::uint32_t>(in.g4933e8)+1U);
    }else if(in.prev_2bd4&0x100U){
        set_2bec(static_cast<std::uint32_t>(static_cast<std::int16_t>(in.h2bd8)>>1));
        out.h2bd8=0xff;
    }else{
        const bool fine=in.b2bdd!=0;
        const float threshold=fine?4.5F:3.0F,scale=fine?51.0F:128.0F;
        if(in.depth<threshold){
            set_2bec(static_cast<std::uint32_t>(cvt_w(ee_arith(scale,ee_arith(in.depth,2.0F,false),true))));
            if(lh_2bec()<=0)set_2bec(1U);
            if(lh_2bec()>=0x80)set_2bec(0x7fU);
            set_globals(static_cast<std::uint32_t>(lh_2bec()));
            set_2bec(static_cast<std::uint32_t>(lh_2bec()+1));
        }else{
            set_globals(0x80U);
            set_2bec(0x80U);
        }
    }
    out.s7=static_cast<std::uint32_t>(lh_2bec());
    return out;
}

std::uint8_t man_draw_mode_00387650(std::uint32_t flags,std::int8_t b2da0,float depth){
    std::uint8_t mode=(flags&0x80000000U)?0:((flags&0x40000000U)?2:1);
    if(mode==2&&b2da0==2)mode=0;
    if(mode==2&&b2da0==0&&depth<2.5F)mode=0;
    return mode;
}

ManDrawTags man_draw_gif_tags_00386ef8(const ManDrawTagsInput& in){
    ManDrawTags out;
    out.h2bd8=in.h2bd8;
    const auto pair=[](std::uint32_t first,std::uint32_t regs,std::uint32_t second,std::uint32_t upper2){
        return std::array<std::uint32_t,8>{0x8000U,first,regs,upper2,0x8000U,second,regs,upper2};
    };
    if(in.a3){
        out.words=pair(0x10424000U,4U,0x10424000U,0x10224000U);
        out.h2bd8=0xff;
    }else if(in.flags_2bd4&0x80U){
        out.words=pair(0x303e4000U,0x412U,in.s2?0x303e4000U:0x307e4000U,0x303e4000U);
    }else if(in.s7<0x80||in.t0){
        out.words=pair(0x302e4000U,0x412U,0x302e4000U,0x302e4000U);
        out.h2bd8=0xff;
    }else{
        out.words=pair(0x302e4000U,0x412U,in.s2?0x302e4000U:0x306e4000U,0x302e4000U);
        out.h2bd8=0xff;
    }
    return out;
}

ManDrawGs man_draw_gs_state(const ManDrawAlpha& alpha){
    if(!alpha.wrote_globals)
        throw std::runtime_error("003872d8: the man draw keeps the previous draw's TEST globals 005382c8..d4 (not hosted)");
    const auto& g=alpha.globals;
    if(g[0]!=1U||g[1]!=5U||g[3]!=3U||g[2]>0xffU)
        throw std::runtime_error("003872d8: man draw TEST globals other than {1, 5, aref, 3} are not hosted");
    ManDrawGs gs;
    gs.vertex_alpha=static_cast<std::uint8_t>(alpha.s7&0xffU);
    gs.aref=static_cast<std::uint8_t>(g[2]);
    return gs;
}

ManDrawPixel man_draw_pixel(std::uint8_t texel_alpha,const ManDrawGs& gs){
    ManDrawPixel p;
    p.as=(std::uint32_t(texel_alpha)*gs.vertex_alpha)>>7;
    p.z_write=p.as>=gs.aref;
    return p;
}
}
