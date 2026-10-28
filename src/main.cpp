#include <chronos/concurrency/pipeline.hpp>
#include <chronos/execution/backtest.hpp>
#include <chronos/market_data/data.hpp>
#include <chronos/replay/replay.hpp>
#include <charconv>
#include <algorithm>
#include <iostream>
#include <string_view>

using namespace chronos;
static std::size_t count_arg(std::string_view text) {
    std::size_t n{}; auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),n);
    if(ec!=std::errc{}||end!=text.data()+text.size()||n>market_data::default_max_events) throw std::invalid_argument("invalid event count");
    return n;
}
static Timestamp duration_arg(std::string_view text) {
    Timestamp n{}; auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),n);
    if(ec!=std::errc{}||end!=text.data()+text.size()||n<=0) throw std::invalid_argument("bar duration must be positive nanoseconds");
    return n;
}
int main(int argc,char** argv) {
    try {
        if(argc==4 && std::string_view(argv[1])=="generate") {
            auto events=market_data::synthetic_bars(count_arg(argv[2]));market_data::save_csv(argv[3],events);
            std::cout<<"generated "<<events.size()<<" deterministic bars\n";return 0;
        }
        if(argc==4 && std::string_view(argv[1])=="pack") {
            auto events=replay::ordered_events(market_data::load_csv(argv[2]));
            market_data::save_binary(argv[3],events,R"({"source":"local CSV","currency":"unspecified","adjustment":"unspecified","price_scale":10000})");
            std::cout<<"packed "<<events.size()<<" events\n";return 0;
        }
        if((argc==3 && (std::string_view(argv[1])=="replay" || std::string_view(argv[1])=="pipeline" || std::string_view(argv[1])=="backtest-tick")) ||
           (argc==4 && std::string_view(argv[1])=="backtest")) {
            const std::string_view command(argv[1]);
            auto events=replay::ordered_events(std::string_view(argv[2]).ends_with(".chr") ? market_data::load_binary(argv[2]).events : market_data::load_csv(argv[2]));
            if(command=="replay" || command=="pipeline") {
                std::uint64_t sum=0;
                const auto handle=[&](const Event& e){sum=sum*1099511628211ULL+e.header.sequence;};
                if(command=="pipeline") concurrency::run_pipeline(std::span<const Event>(events),handle);
                else { replay::Replay replay(std::move(events));replay.run(handle); }
                std::cout<<"{\"checksum\":"<<sum<<"}\n"; return 0;
            }
            if(command=="backtest" || command=="backtest-tick") {
                ExecutionConfig config;config.model=command=="backtest-tick"?ExecutionModel::Tick:ExecutionModel::Bar;
                if(config.model==ExecutionModel::Bar) config.bar_duration_ns=duration_arg(argv[3]);
                config.commission_per_unit=10;config.slippage_bps=1;
                BuyAndHold strategy;auto result=run_backtest(events,strategy,1'000'000*price_scale,config);
                const auto& p=result.portfolio;
                const auto fills=std::count_if(result.reports.begin(),result.reports.end(),[](const auto& r){return r.quantity>0 && (r.status==ExecutionStatus::Filled || r.status==ExecutionStatus::Partial);});
                std::cout<<"{\"cash_fixed\":"<<p.cash()<<",\"equity_fixed\":"<<p.equity()<<",\"fees_fixed\":"<<p.fees()<<",\"fills\":"<<fills<<",\"execution_reports\":"<<result.reports.size()<<",\"pending\":"<<result.pending_orders<<"}\n";return 0;
            }
        }
        std::cerr<<"usage: chronos_cli generate COUNT OUTPUT.csv | pack INPUT.csv OUTPUT.chr | replay FILE | pipeline FILE | backtest FILE BAR_DURATION_NS | backtest-tick FILE\n";
        return 2;
    } catch(const std::exception& e) {std::cerr<<"chronos: "<<e.what()<<'\n';return 1;}
}
