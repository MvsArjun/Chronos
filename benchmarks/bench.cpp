#include <benchmark/benchmark.h>
#include <chronos/concurrency/pipeline.hpp>
#include <chronos/execution/backtest.hpp>
#include <chronos/market_data/data.hpp>
#include <chronos/replay/replay.hpp>
#include <array>
#include <filesystem>
#include <memory_resource>

using namespace chronos;
static const auto events = market_data::synthetic_bars(10'000);
static void Dispatch(benchmark::State& state) {
    for(auto _ : state) { std::uint64_t sum=0;for(const auto& e:events) sum+=e.header.sequence;benchmark::DoNotOptimize(sum); }
    state.SetItemsProcessed(state.iterations()*static_cast<std::int64_t>(events.size()));
}
BENCHMARK(Dispatch);
static void SpscRoundTrip(benchmark::State& state) {
    concurrency::SpscRing<Event,1024> q;
    for(auto _ : state) { q.try_push(events[0]);auto event=q.try_pop();benchmark::DoNotOptimize(event); }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(SpscRoundTrip);
static void Pipeline(benchmark::State& state) {
    for(auto _ : state) {std::uint64_t sum=0;concurrency::run_pipeline(std::span<const Event>(events),[&](const Event&e){sum+=e.header.sequence;});benchmark::DoNotOptimize(sum);}
    state.SetItemsProcessed(state.iterations()*static_cast<std::int64_t>(events.size()));
}
BENCHMARK(Pipeline)->UseRealTime();
static void ReplayDispatch(benchmark::State& state) {
    replay::Replay r(events);
    for(auto _ : state) {std::uint64_t sum=0;r.run([&](const Event&e){sum+=e.header.sequence;});benchmark::DoNotOptimize(sum);}
    state.SetItemsProcessed(state.iterations()*static_cast<std::int64_t>(events.size()));
}
BENCHMARK(ReplayDispatch);
static void ReplayPrepare(benchmark::State& state) {
    for(auto _ : state) {replay::Replay r(events);benchmark::DoNotOptimize(r.events().data());}
    state.SetItemsProcessed(state.iterations()*static_cast<std::int64_t>(events.size()));
}
BENCHMARK(ReplayPrepare);
static void Backtest(benchmark::State& state) {
    for(auto _ : state) {BuyAndHold s;auto result=run_backtest(events,s);benchmark::DoNotOptimize(result.portfolio.cash());}
    state.SetItemsProcessed(state.iterations()*static_cast<std::int64_t>(events.size()));
}
BENCHMARK(Backtest);
static void CsvParse(benchmark::State& state) {
    const auto path=std::filesystem::temp_directory_path()/"chronos_bench.csv";
    market_data::save_csv(path,events);
    for(auto _ : state) {auto input=market_data::load_csv(path);benchmark::DoNotOptimize(input.data());}
    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations()*static_cast<std::int64_t>(events.size()));
}
BENCHMARK(CsvParse);
static void BinaryRead(benchmark::State& state) {
    const auto path=std::filesystem::temp_directory_path()/"chronos_bench.chr";
    market_data::save_binary(path,events);
    for(auto _ : state) {auto input=market_data::load_binary(path);benchmark::DoNotOptimize(input.events.data());}
    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations()*static_cast<std::int64_t>(events.size()));
}
BENCHMARK(BinaryRead);
struct CountingResource final : std::pmr::memory_resource {
    std::size_t allocations{},bytes{};
    void* do_allocate(std::size_t n,std::size_t align) override {++allocations;bytes+=n;return std::pmr::new_delete_resource()->allocate(n,align);}
    void do_deallocate(void* p,std::size_t n,std::size_t align) override {std::pmr::new_delete_resource()->deallocate(p,n,align);}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {return this==&other;}
};
static void EventAllocation(benchmark::State& state) {
    std::array<std::byte,4096*sizeof(Event)+64> buffer;
    CountingResource resource;
    for(auto _ : state) {
        std::pmr::monotonic_buffer_resource arena(buffer.data(),buffer.size(),&resource);
        std::pmr::vector<Event> data(state.range(0)==2 ? static_cast<std::pmr::memory_resource*>(&arena) : &resource);
        if(state.range(0)!=0) data.reserve(4096);
        for(int i=0;i<4096;++i) data.push_back(events[static_cast<std::size_t>(i)]);
        benchmark::DoNotOptimize(data.data());benchmark::ClobberMemory();
    }
    state.counters["container_allocations_per_batch"]=static_cast<double>(resource.allocations)/static_cast<double>(state.iterations());
    state.counters["container_bytes_per_batch"]=static_cast<double>(resource.bytes)/static_cast<double>(state.iterations());
    state.SetItemsProcessed(state.iterations()*4096);
}
BENCHMARK(EventAllocation)->Arg(0)->Arg(1)->Arg(2);
