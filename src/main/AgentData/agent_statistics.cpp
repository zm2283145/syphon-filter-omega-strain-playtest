#include "agent_statistics.hpp"

namespace omega {

void add_agent_statistic(AgentStatistics& s,std::uint32_t index,std::int32_t amount){
    const std::uint32_t i=index&0xffU;                                       // 00333f10 andi 0xff
    if(i>=0x1fU||amount<=0)return;                                          // slti 0x1f, blez
    auto wrap=[](std::int32_t a,std::int32_t b){                             // addu (32-bit wrap)
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a)+static_cast<std::uint32_t>(b));};
    s.statistics[i]=wrap(s.statistics[i],amount);
    if(i<0xdU)s.statistics[i+0x12U]=wrap(s.statistics[i+0x12U],amount);    // +50 + 4i
}

std::int32_t read_agent_statistic(const AgentStatistics& s,std::uint32_t index){
    const std::uint32_t i=index&0xffU;                                       // 00333ee0
    return i<0x1fU?s.statistics[i]:0;
}

void reset_agent_level_statistics(AgentStatistics& s){
    for(std::size_t i=0;i<0xd;++i)                                          // 00333df0: +8 + 4i -= +50 + 4i
        s.statistics[i]=static_cast<std::int32_t>(static_cast<std::uint32_t>(s.statistics[i])-static_cast<std::uint32_t>(s.statistics[i+0x12]));
    for(std::size_t i=0x12;i<0x1f;++i)s.statistics[i]=0;                    // +50 .. +80
    s.words_a4.fill(0);                                                      // +a4 .. +c0
}

}
