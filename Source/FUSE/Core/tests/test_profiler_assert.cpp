#include <fuse/assert.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/profiler/profiler.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void resetState() {
    fuse::profiler::reset();
    fuse::profiler::setEnabled(true);
    fuse::assertion::clearFatalHandler();
    fuse::assertion::setSuppressAbortForTests(false);
}

void testProfileScopeRecordsEvents() {
    resetState();

    {
        FUSE_PROFILE_SCOPE("test_scope");
    }

    expectTrue(fuse::profiler::eventCount() >= 2u, "scope records begin and end events");
    const fuse::profiler::ProfileEvent& begin = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& end = fuse::profiler::eventAt(1);
    expectTrue(begin.phase == fuse::profiler::EventPhase::Begin, "first event is begin");
    expectTrue(end.phase == fuse::profiler::EventPhase::End, "second event is end");
    expectTrue(begin.name != nullptr && std::string(begin.name) == "test_scope", "scope name preserved");
    expectTrue(end.timestampNs >= begin.timestampNs, "end timestamp is not before begin");
}

void testProfilerDisabledSkipsEvents() {
    resetState();
    fuse::profiler::setEnabled(false);

    {
        FUSE_PROFILE_SCOPE("ignored");
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler records nothing");
}

void testFrameBoundaryIncrementsIndex() {
    resetState();

    const fuse::u32 before = fuse::profiler::frameIndex();
    fuse::profiler::beginFrame();
    fuse::profiler::endFrame();
    expectTrue(fuse::profiler::frameIndex() == before + 1u, "beginFrame increments frame index");
}

void testNestedScopeOrdering() {
    resetState();

    {
        FUSE_PROFILE_SCOPE("outer");
        {
            FUSE_PROFILE_SCOPE("inner");
            {
                FUSE_PROFILE_SCOPE("deepest");
            }
        }
    }

    expectTrue(fuse::profiler::eventCount() == 6u, "three nested scopes emit six events");
    expectTrue(fuse::profiler::maxNestingDepth() == 3u, "max nesting depth tracks deepest scope");

    const fuse::profiler::ProfileEvent& e0 = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& e1 = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& e2 = fuse::profiler::eventAt(2);
    const fuse::profiler::ProfileEvent& e3 = fuse::profiler::eventAt(3);
    const fuse::profiler::ProfileEvent& e4 = fuse::profiler::eventAt(4);
    const fuse::profiler::ProfileEvent& e5 = fuse::profiler::eventAt(5);

    expectTrue(e0.phase == fuse::profiler::EventPhase::Begin && std::string(e0.name) == "outer",
               "nested trace begins with outer");
    expectTrue(e1.phase == fuse::profiler::EventPhase::Begin && std::string(e1.name) == "inner",
               "nested trace continues with inner begin");
    expectTrue(e2.phase == fuse::profiler::EventPhase::Begin && std::string(e2.name) == "deepest",
               "nested trace reaches deepest begin");
    expectTrue(e3.phase == fuse::profiler::EventPhase::End && std::string(e3.name) == "deepest",
               "deepest scope ends before inner");
    expectTrue(e4.phase == fuse::profiler::EventPhase::End && std::string(e4.name) == "inner",
               "inner scope ends before outer");
    expectTrue(e5.phase == fuse::profiler::EventPhase::End && std::string(e5.name) == "outer",
               "outer scope ends last");

    expectTrue(e0.nestingDepth == 1u && e1.nestingDepth == 2u && e2.nestingDepth == 3u,
               "nesting depth increments per scope level");
    expectTrue(e0.scopeId == e5.scopeId, "outer begin/end share scope id");
    expectTrue(e2.scopeId == e3.scopeId, "deepest begin/end share scope id");
}

void testThreadIdStubs() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("main_thread_scope");
    }

    const fuse::profiler::ProfileEvent& begin = fuse::profiler::eventAt(0);
    expectTrue(begin.threadId != 0u, "profiler records non-zero chrome tid");
    expectTrue(begin.threadId == fuse::platform::chromeTraceThreadId(),
               "profiler tid matches platform chromeTraceThreadId");
    expectTrue(fuse::platform::isMainThread(), "main thread stub reports current thread as main");

    std::atomic<bool> workerDone{false};
    std::atomic<fuse::u32> workerThreadTid{0};
    std::thread worker([&]() {
        workerThreadTid.store(fuse::platform::chromeTraceThreadId(), std::memory_order_release);
        FUSE_PROFILE_SCOPE("worker_scope");
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(workerThreadTid.load(std::memory_order_acquire) != begin.threadId,
               "worker thread tid differs from main thread tid");
    expectTrue(fuse::profiler::eventCount() >= 4u, "worker scope adds begin/end events");

    const fuse::profiler::ProfileEvent& workerBegin = fuse::profiler::eventAt(2);
    expectTrue(workerBegin.threadId == workerThreadTid.load(std::memory_order_acquire),
               "worker event tid matches worker chromeTraceThreadId");
    expectTrue(std::string(workerBegin.name) == "worker_scope", "worker scope name preserved");
}

void testChromeTraceExport() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("chrome_scope");
        {
            FUSE_PROFILE_SCOPE("chrome_child");
        }
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"displayTimeUnit\":\"ns\"") != std::string::npos,
               "chrome trace declares displayTimeUnit");
    expectTrue(json.find("\"metadata\":{\"name\":\"FUSE CPU profiler\"") != std::string::npos,
               "chrome trace includes metadata block");
    expectTrue(json.find("\"frame\":") != std::string::npos, "chrome trace exports frame metadata");
    expectTrue(json.find("\"traceEvents\":[") != std::string::npos, "chrome trace has traceEvents root");
    expectTrue(json.find("\"name\":\"chrome_scope\"") != std::string::npos, "chrome trace includes scope name");
    expectTrue(json.find("\"name\":\"chrome_child\"") != std::string::npos, "chrome trace includes nested scope");
    expectTrue(json.find("\"ph\":\"B\"") != std::string::npos, "chrome trace has begin phase");
    expectTrue(json.find("\"ph\":\"E\"") != std::string::npos, "chrome trace has end phase");
    expectTrue(json.find("\"pid\":1") != std::string::npos, "chrome trace pins process id");
    expectTrue(json.find("\"tid\":") != std::string::npos, "chrome trace exports thread id");
    expectTrue(json.find("\"id\":") != std::string::npos, "chrome trace pairs begin/end with id");
    expectTrue(json.find("\"args\":{\"depth\":") != std::string::npos, "chrome trace exports nesting depth args");
    expectTrue(json.back() == '}', "chrome trace json is closed");
}

void testAsyncFlowStubs() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    expectTrue(flowId != 0u, "nextFlowId returns non-zero id");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("vfs_load", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("vfs_load", flowId);

    expectTrue(fuse::profiler::eventCount() == 2u, "async flow emits start and finish events");

    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& flowFinish = fuse::profiler::eventAt(1);
    expectTrue(flowStart.phase == fuse::profiler::EventPhase::FlowStart, "first flow event is start");
    expectTrue(flowFinish.phase == fuse::profiler::EventPhase::FlowFinish, "second flow event is finish");
    expectTrue(std::string(flowStart.name) == "vfs_load", "flow start preserves name");
    expectTrue(std::string(flowFinish.name) == "vfs_load", "flow finish preserves name");
    expectTrue(flowStart.scopeId == flowId, "flow start stores flow id");
    expectTrue(flowFinish.scopeId == flowId, "flow finish stores flow id");
    expectTrue(flowFinish.timestampNs >= flowStart.timestampNs,
               "flow finish timestamp is not before flow start");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"cat\":\"async\"") != std::string::npos, "async flow uses async category");
    expectTrue(json.find("\"ph\":\"s\"") != std::string::npos, "chrome trace exports flow start phase");
    expectTrue(json.find("\"ph\":\"f\"") != std::string::npos, "chrome trace exports flow finish phase");
    expectTrue(json.find("\"name\":\"vfs_load\"") != std::string::npos, "chrome trace includes flow name");
    expectTrue(json.find("\"bp\":\"e\"") != std::string::npos, "flow finish binds to enclosing slice end");
}

void testAsyncFlowCrossThread() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = 42u;
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("job_handoff", flowId);

    std::atomic<bool> workerDone{false};
    std::atomic<fuse::u32> workerTid{0};
    std::thread worker([&]() {
        workerTid.store(fuse::platform::chromeTraceThreadId(), std::memory_order_release);
        FUSE_PROFILE_ASYNC_FLOW_END("job_handoff", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(fuse::profiler::eventCount() == 2u, "cross-thread flow emits two events");
    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& flowFinish = fuse::profiler::eventAt(1);
    expectTrue(flowStart.threadId != flowFinish.threadId, "flow start and finish record distinct tids");
    expectTrue(flowFinish.threadId == workerTid.load(std::memory_order_acquire),
               "flow finish tid matches worker chromeTraceThreadId");
}

void testCounterSamples() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("frame_alloc_bytes", 4096);
    FUSE_PROFILE_COUNTER("active_jobs", 3);

    expectTrue(fuse::profiler::eventCount() == 2u, "counter samples emit one event each");

    const fuse::profiler::ProfileEvent& first = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& second = fuse::profiler::eventAt(1);
    expectTrue(first.phase == fuse::profiler::EventPhase::Counter, "counter event phase is Counter");
    expectTrue(second.phase == fuse::profiler::EventPhase::Counter, "second counter event phase is Counter");
    expectTrue(std::string(first.name) == "frame_alloc_bytes", "counter track name preserved");
    expectTrue(first.counterKind == fuse::profiler::CounterValueKind::Int, "int counter kind preserved");
    expectTrue(first.counterIntValue == 4096, "counter value preserved");
    expectTrue(second.counterIntValue == 3, "second counter value preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"cat\":\"counter\"") != std::string::npos, "counter uses counter category");
    expectTrue(json.find("\"ph\":\"C\"") != std::string::npos, "chrome trace exports counter phase");
    expectTrue(json.find("\"name\":\"frame_alloc_bytes\"") != std::string::npos,
               "chrome trace includes counter track name");
    expectTrue(json.find("\"args\":{\"value\":4096}") != std::string::npos,
               "chrome trace exports counter value args");
    expectTrue(json.find("\"args\":{\"value\":3}") != std::string::npos,
               "chrome trace exports second counter value");
}

void testFloatCounterSamples() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("frame_time_ms", 16.667);
    FUSE_PROFILE_COUNTER("gpu_utilization", 0.75);

    expectTrue(fuse::profiler::eventCount() == 2u, "float counter samples emit one event each");

    const fuse::profiler::ProfileEvent& first = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& second = fuse::profiler::eventAt(1);
    expectTrue(first.phase == fuse::profiler::EventPhase::Counter, "float counter event phase is Counter");
    expectTrue(first.counterKind == fuse::profiler::CounterValueKind::Float, "float counter kind preserved");
    expectTrue(first.counterFloatValue > 16.666 && first.counterFloatValue < 16.668,
               "float counter value preserved");
    expectTrue(second.counterFloatValue == 0.75, "second float counter value preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"frame_time_ms\"") != std::string::npos,
               "chrome trace includes float counter track name");
    expectTrue(json.find("\"args\":{\"value\":16.667") != std::string::npos,
               "chrome trace exports float counter value args");
    expectTrue(json.find("\"args\":{\"value\":0.75}") != std::string::npos,
               "chrome trace exports second float counter value");
}

void testAsyncFlowMatchingIdsInExport() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = 77u;
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("paired_flow", flowId);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    const std::string flowIdToken = "\"id\":" + std::to_string(flowId);
    const auto firstId = json.find(flowIdToken);
    const auto secondId = json.find(flowIdToken, firstId == std::string::npos ? 0u : firstId + 1u);
    expectTrue(firstId != std::string::npos, "flow start exports matching flow id");
    expectTrue(secondId != std::string::npos, "flow finish exports matching flow id");
    expectTrue(json.find("\"ph\":\"s\"") != std::string::npos && json.find("\"ph\":\"f\"") != std::string::npos,
               "paired flow exports start and finish phases");
}

void testChromeTraceNestingDepthExport() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("depth_outer");
        {
            FUSE_PROFILE_SCOPE("depth_inner");
        }
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"depth\":1}") != std::string::npos,
               "chrome trace exports outer scope depth");
    expectTrue(json.find("\"args\":{\"depth\":2}") != std::string::npos,
               "chrome trace exports inner scope depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 2u, "nesting depth tracks two-level stack");
}

void testChromeTraceEmptyExport() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::eventCount() == 0u, "reset leaves profiler buffer empty");
    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"displayTimeUnit\":\"ns\"") != std::string::npos,
               "empty export declares displayTimeUnit");
    expectTrue(json.find("\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":0}") != std::string::npos,
               "empty export includes frame metadata");
    expectTrue(json.find("\"traceEvents\":[]") != std::string::npos,
               "empty export emits zero trace events");
    expectTrue(json.back() == '}', "empty export json is closed");
}

void testNestedAsyncFlowWithinScope() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("flow_outer");
        {
            FUSE_PROFILE_SCOPE("flow_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("nested_io", flowId);
            FUSE_PROFILE_COUNTER("nested_budget", 12);
            FUSE_PROFILE_ASYNC_FLOW_END("nested_io", flowId);
        }
    }

    expectTrue(fuse::profiler::eventCount() == 7u,
               "nested flow inside scopes emits scope + flow + counter events");

    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(2);
    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(3);
    const fuse::profiler::ProfileEvent& flowFinish = fuse::profiler::eventAt(4);
    expectTrue(flowStart.phase == fuse::profiler::EventPhase::FlowStart, "nested flow start recorded");
    expectTrue(flowFinish.phase == fuse::profiler::EventPhase::FlowFinish, "nested flow finish recorded");
    expectTrue(flowStart.nestingDepth == 2u, "flow start inherits inner scope depth");
    expectTrue(flowFinish.nestingDepth == 2u, "flow finish inherits inner scope depth");
    expectTrue(counter.nestingDepth == 2u, "counter sample inherits inner scope depth");
    expectTrue(counter.flowNestingDepth == 1u, "counter sample inherits async flow depth");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"nested_io\"") != std::string::npos,
               "nested flow export keeps flow name");
    expectTrue(json.find("\"args\":{\"depth\":2}") != std::string::npos,
               "nested flow export includes inner scope depth");
    expectTrue(json.find("\"args\":{\"value\":12,\"depth\":2,\"flow_depth\":1}") != std::string::npos,
               "nested counter export includes scope and flow depth");
}

void testDisabledProfilerSkipsAsyncFlowAndCounter() {
    resetState();
    fuse::profiler::setEnabled(false);

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_flow", flowId);
    FUSE_PROFILE_COUNTER("ignored_counter", 99);
    FUSE_PROFILE_ASYNC_FLOW_END("ignored_flow", flowId);

    expectTrue(fuse::profiler::eventCount() == 0u,
               "disabled profiler skips async flow and counter samples");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "disabled profiler export stays empty");
}

void testChromeTraceEscapedNameExport() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("track\"quoted\\path", 1);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"track\\\"quoted\\\\path\"") != std::string::npos,
               "chrome export escapes quotes and backslashes in counter track names");
}

void testChromeTraceEscapedControlCharsExport() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("line\nbreak\ttab", 2);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"line\\nbreak\\ttab\"") != std::string::npos,
               "chrome export escapes control characters in counter track names");
}

void testChromeTraceEscapedScopeAndFlowNames() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("scope\"name\\v1");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow\nstart", flowId);
        FUSE_PROFILE_ASYNC_FLOW_END("flow\nstart", flowId);
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"scope\\\"name\\\\v1\"") != std::string::npos,
               "chrome export escapes quotes and backslashes in scope names");
    expectTrue(json.find("\"name\":\"flow\\nstart\"") != std::string::npos,
               "chrome export escapes newlines in async flow names");
}

void testCounterSampleInsideScopeDepthExport() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("budget_parent");
        FUSE_PROFILE_COUNTER("scoped_budget", 42);
    }

    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(1);
    expectTrue(counter.phase == fuse::profiler::EventPhase::Counter, "counter event recorded inside scope");
    expectTrue(counter.nestingDepth == 1u, "counter inside one scope inherits depth 1");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":42,\"depth\":1}") != std::string::npos,
               "chrome export includes depth for counter sampled inside scope");
}

void testMultipleAsyncFlowsInNestedScopes() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    expectTrue(innerFlowId > outerFlowId, "nextFlowId is monotonic");

    {
        FUSE_PROFILE_SCOPE("multi_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("outer_io", outerFlowId);
        {
            FUSE_PROFILE_SCOPE("multi_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("inner_io", innerFlowId);
            FUSE_PROFILE_ASYNC_FLOW_END("inner_io", innerFlowId);
        }
        FUSE_PROFILE_ASYNC_FLOW_END("outer_io", outerFlowId);
    }

    expectTrue(fuse::profiler::eventCount() == 8u,
               "nested concurrent flows emit scope + paired flow events");

    const fuse::profiler::ProfileEvent& outerStart = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& innerStart = fuse::profiler::eventAt(3);
    const fuse::profiler::ProfileEvent& innerFinish = fuse::profiler::eventAt(4);
    const fuse::profiler::ProfileEvent& outerFinish = fuse::profiler::eventAt(6);
    expectTrue(outerStart.phase == fuse::profiler::EventPhase::FlowStart, "outer flow start recorded");
    expectTrue(innerStart.phase == fuse::profiler::EventPhase::FlowStart, "inner flow start recorded");
    expectTrue(innerFinish.phase == fuse::profiler::EventPhase::FlowFinish, "inner flow finishes before outer");
    expectTrue(outerFinish.phase == fuse::profiler::EventPhase::FlowFinish, "outer flow finish follows inner scope end");
    expectTrue(outerStart.nestingDepth == 1u, "outer flow inherits outer scope depth");
    expectTrue(innerStart.nestingDepth == 2u, "inner flow inherits inner scope depth");
    expectTrue(outerStart.scopeId == outerFlowId, "outer flow id preserved");
    expectTrue(innerStart.scopeId == innerFlowId, "inner flow id preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    const std::string outerIdToken = "\"id\":" + std::to_string(outerFlowId);
    const std::string innerIdToken = "\"id\":" + std::to_string(innerFlowId);
    expectTrue(json.find(outerIdToken) != std::string::npos, "outer flow id exported");
    expectTrue(json.find(innerIdToken) != std::string::npos, "inner flow id exported");
    expectTrue(json.find("\"name\":\"outer_io\"") != std::string::npos, "outer flow name exported");
    expectTrue(json.find("\"name\":\"inner_io\"") != std::string::npos, "inner flow name exported");
}

void testChromeTraceExportFrameIndex() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::beginFrame();
    fuse::profiler::endFrame();

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":2}") != std::string::npos,
               "chrome export frame metadata reflects beginFrame count");
}

void testChromeTraceExportMixedEvents() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("mixed_parent");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("mixed_flow", flowId);
        FUSE_PROFILE_COUNTER("mixed_counter", 7);
        FUSE_PROFILE_ASYNC_FLOW_END("mixed_flow", flowId);
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"ph\":\"B\"") != std::string::npos, "mixed export keeps scope begin");
    expectTrue(json.find("\"ph\":\"E\"") != std::string::npos, "mixed export keeps scope end");
    expectTrue(json.find("\"ph\":\"s\"") != std::string::npos, "mixed export keeps flow start");
    expectTrue(json.find("\"ph\":\"f\"") != std::string::npos, "mixed export keeps flow finish");
    expectTrue(json.find("\"ph\":\"C\"") != std::string::npos, "mixed export keeps counter sample");
    expectTrue(json.find("\"name\":\"mixed_parent\"") != std::string::npos,
               "mixed export keeps parent scope name");
    expectTrue(json.find("\"name\":\"mixed_flow\"") != std::string::npos,
               "mixed export keeps flow name");
    expectTrue(json.find("\"name\":\"mixed_counter\"") != std::string::npos,
               "mixed export keeps counter track name");
}

void testFatalHandlerHook() {
    resetState();

    struct FatalCaptureState {
        bool called = false;
        fuse::assertion::FatalContext context{};
    } captureState;

    fuse::assertion::setFatalHandler(
        [](const fuse::assertion::FatalContext& context, void* userData) {
            auto* state = static_cast<FatalCaptureState*>(userData);
            state->called = true;
            state->context = context;
        },
        &captureState);

    fuse::assertion::setSuppressAbortForTests(true);
    fuse::assertion::fatal("verify failed", "test_profiler_assert.cpp", 99u);

    expectTrue(captureState.called, "fatal handler invoked");
    expectTrue(captureState.context.message != nullptr
                   && std::string(captureState.context.message) == "verify failed",
               "fatal handler receives message");
    expectTrue(captureState.context.line == 99u, "fatal handler receives line");
}

void testHasEventsAndEmptyBufferGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::hasEvents(), "reset leaves hasEvents false");
    expectTrue(fuse::profiler::eventCount() == 0u, "reset leaves event count at zero");

    const fuse::profiler::ProfileEvent& emptyEvent = fuse::profiler::eventAt(0);
    expectTrue(emptyEvent.name == nullptr, "eventAt on empty buffer returns sentinel with null name");
    expectTrue(emptyEvent.phase == fuse::profiler::EventPhase::Begin,
               "eventAt sentinel keeps default begin phase");

    const fuse::profiler::ProfileEvent& oobEvent = fuse::profiler::eventAt(99);
    expectTrue(oobEvent.name == nullptr, "eventAt out-of-range returns sentinel with null name");

    {
        FUSE_PROFILE_SCOPE("guard_scope");
    }

    expectTrue(fuse::profiler::hasEvents(), "hasEvents true after recording scope");
    expectTrue(fuse::profiler::eventAt(0).name != nullptr, "eventAt(0) valid after recording");
    expectTrue(fuse::profiler::eventAt(2).name == nullptr, "eventAt past count returns sentinel");
}

void testNestedAsyncFlowDepth() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("outer_flow", outerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("inner_flow", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("inner_flow", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("outer_flow", outerFlowId);

    expectTrue(fuse::profiler::eventCount() == 4u, "nested flows emit four events");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 2u, "max flow nesting depth tracks inner flow");

    const fuse::profiler::ProfileEvent& outerStart = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& innerStart = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& innerFinish = fuse::profiler::eventAt(2);
    const fuse::profiler::ProfileEvent& outerFinish = fuse::profiler::eventAt(3);

    expectTrue(outerStart.flowNestingDepth == 1u, "outer flow start records depth 1");
    expectTrue(innerStart.flowNestingDepth == 2u, "inner flow start records depth 2");
    expectTrue(innerFinish.flowNestingDepth == 2u, "inner flow finish records depth 2");
    expectTrue(outerFinish.flowNestingDepth == 1u, "outer flow finish records depth 1");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"flow_depth\":1}") != std::string::npos,
               "chrome export includes outer flow depth");
    expectTrue(json.find("\"args\":{\"flow_depth\":2}") != std::string::npos,
               "chrome export includes inner flow depth");
}

void testCounterSnapshotAtFrame() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::beginFrame();
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("frame_budget", 8192);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("frame_time_ms", 12.5);

    expectTrue(fuse::profiler::eventCount() == 2u, "snapshot_at_frame counters emit two events");

    const fuse::profiler::ProfileEvent& intCounter = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& floatCounter = fuse::profiler::eventAt(1);
    expectTrue(intCounter.counterSnapshotFrame == 2u, "int counter records current frame index");
    expectTrue(floatCounter.counterSnapshotFrame == 2u, "float counter records current frame index");
    expectTrue(intCounter.counterIntValue == 8192, "int counter value preserved with frame snapshot");
    expectTrue(floatCounter.counterKind == fuse::profiler::CounterValueKind::Float,
               "float snapshot counter kind preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":8192,\"snapshot_at_frame\":2}") != std::string::npos,
               "chrome export includes int counter snapshot_at_frame");
    expectTrue(json.find("\"args\":{\"value\":12.5,\"snapshot_at_frame\":2}") != std::string::npos,
               "chrome export includes float counter snapshot_at_frame");
}

void testCounterSnapshotAtFrameInsideScope() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("budget_scope");
        FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("scoped_frame_budget", 128);
    }

    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(1);
    expectTrue(counter.nestingDepth == 1u, "snapshot counter inside scope inherits depth");
    expectTrue(counter.counterSnapshotFrame == 1u, "snapshot counter inside scope records frame index");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":128,\"depth\":1,\"snapshot_at_frame\":1}") != std::string::npos,
               "chrome export includes depth and snapshot_at_frame for scoped counter");
}

void testChromeTraceEscapedLowControlChars() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("bell\x07" "char", 1);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"bell\\u0007char\"") != std::string::npos,
               "chrome export escapes low control characters as unicode");
}

void testNestedFlowInsideScopeExportsBothDepths() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("flow_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("scoped_flow", flowId);
        FUSE_PROFILE_ASYNC_FLOW_END("scoped_flow", flowId);
    }

    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(1);
    expectTrue(flowStart.nestingDepth == 1u, "flow inside scope inherits scope depth");
    expectTrue(flowStart.flowNestingDepth == 1u, "flow inside scope records flow depth");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"depth\":1,\"flow_depth\":1}") != std::string::npos,
               "chrome export includes scope and flow depth for nested flow");
}

void testDisabledProfilerDoesNotMutateFlowNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_outer", flowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_inner", flowId + 1u);
    FUSE_PROFILE_ASYNC_FLOW_END("ignored_inner", flowId + 1u);
    FUSE_PROFILE_ASYNC_FLOW_END("ignored_outer", flowId);

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested async flow events");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 0u,
               "disabled profiler does not mutate max flow nesting depth");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("enabled_flow", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("enabled_flow", flowId);
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 1u,
               "flow nesting depth resumes cleanly after re-enable");
}

void testCounterInsideNestedFlowRecordsFlowDepth() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("budget_outer", outerFlowId);
    FUSE_PROFILE_COUNTER("outer_budget", 10);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("budget_inner", innerFlowId);
    FUSE_PROFILE_COUNTER("inner_budget", 20);
    FUSE_PROFILE_ASYNC_FLOW_END("budget_inner", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("budget_outer", outerFlowId);

    const fuse::profiler::ProfileEvent& outerCounter = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& innerCounter = fuse::profiler::eventAt(3);
    expectTrue(outerCounter.flowNestingDepth == 1u, "counter in outer flow records flow depth 1");
    expectTrue(innerCounter.flowNestingDepth == 2u, "counter in inner flow records flow depth 2");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"outer_budget\"") != std::string::npos,
               "nested flow counter export keeps outer track name");
    expectTrue(json.find("\"args\":{\"value\":10,\"flow_depth\":1}") != std::string::npos,
               "chrome export includes flow_depth for outer counter");
    expectTrue(json.find("\"args\":{\"value\":20,\"flow_depth\":2}") != std::string::npos,
               "chrome export includes flow_depth for inner counter");
}

void testSnapshotAtFrameCounterInsideNestedFlowAndScope() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::beginFrame();
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("snapshot_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("snapshot_flow", flowId);
        FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("combined_budget", 256);
        FUSE_PROFILE_ASYNC_FLOW_END("snapshot_flow", flowId);
    }

    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(2);
    expectTrue(counter.nestingDepth == 1u, "snapshot counter inherits scope depth");
    expectTrue(counter.flowNestingDepth == 1u, "snapshot counter inherits flow depth");
    expectTrue(counter.counterSnapshotFrame == 2u, "snapshot counter records frame index");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":256,\"depth\":1,\"flow_depth\":1,\"snapshot_at_frame\":2}")
                   != std::string::npos,
               "chrome export includes scope depth, flow depth, and snapshot_at_frame");
}

void testIsEmptyAndIsValidEventIndex() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::isEmpty(), "reset leaves profiler buffer empty");
    expectTrue(!fuse::profiler::hasEvents(), "isEmpty mirrors hasEvents on reset");
    expectTrue(!fuse::profiler::isValidEventIndex(0), "index 0 invalid on empty buffer");
    expectTrue(!fuse::profiler::isValidEventIndex(99), "out-of-range index invalid on empty buffer");

    FUSE_PROFILE_COUNTER("probe", 1);
    expectTrue(!fuse::profiler::isEmpty(), "counter sample clears isEmpty");
    expectTrue(fuse::profiler::isValidEventIndex(0), "index 0 valid after recording");
    expectTrue(!fuse::profiler::isValidEventIndex(1), "index past count invalid");
    expectTrue(!fuse::profiler::isValidEventIndex(99), "far out-of-range index invalid");
}

void testEmptyEventSentinel() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ProfileEvent& sentinel = fuse::profiler::emptyEvent();
    expectTrue(sentinel.name == nullptr, "emptyEvent sentinel has null name");
    expectTrue(&fuse::profiler::eventAt(0) == &sentinel, "eventAt on empty buffer returns emptyEvent");
    expectTrue(&fuse::profiler::eventAt(42) == &sentinel, "eventAt out-of-range returns emptyEvent");

    FUSE_PROFILE_SCOPE("sentinel_scope");
    expectTrue(fuse::profiler::eventAt(0).name != nullptr, "eventAt(0) valid after recording");
    expectTrue(&fuse::profiler::eventAt(99) == &sentinel, "eventAt past count still returns emptyEvent");
}

void testActiveDepthQueries() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::activeNestingDepth() == 0u, "active scope depth starts at zero");
    expectTrue(fuse::profiler::activeFlowNestingDepth() == 0u, "active flow depth starts at zero");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("depth_outer");
        expectTrue(fuse::profiler::activeNestingDepth() == 1u, "outer scope raises active nesting depth");
        {
            FUSE_PROFILE_SCOPE("depth_inner");
            expectTrue(fuse::profiler::activeNestingDepth() == 2u, "inner scope raises active nesting depth");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("depth_flow", flowId);
            expectTrue(fuse::profiler::activeFlowNestingDepth() == 1u,
                       "flow begin raises active flow nesting depth");
            FUSE_PROFILE_COUNTER("depth_counter", 5);
            FUSE_PROFILE_ASYNC_FLOW_END("depth_flow", flowId);
            expectTrue(fuse::profiler::activeFlowNestingDepth() == 0u,
                       "flow end restores active flow nesting depth");
        }
        expectTrue(fuse::profiler::activeNestingDepth() == 1u, "inner scope exit restores outer depth");
    }
    expectTrue(fuse::profiler::activeNestingDepth() == 0u, "all scopes closed restores zero depth");
}

void testCrossThreadFlowDepthIsThreadLocal() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = 42u;
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("job_handoff", flowId);
    expectTrue(fuse::profiler::activeFlowNestingDepth() == 1u,
               "flow begin raises depth on originating thread only");

    std::atomic<bool> workerDone{false};
    std::atomic<fuse::u32> workerFlowDepth{0};
    std::thread worker([&]() {
        workerFlowDepth.store(fuse::profiler::activeFlowNestingDepth(), std::memory_order_release);
        FUSE_PROFILE_ASYNC_FLOW_END("job_handoff", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(workerFlowDepth.load(std::memory_order_acquire) == 0u,
               "worker thread flow depth stays zero before cross-thread end");
    expectTrue(fuse::profiler::activeFlowNestingDepth() == 1u,
               "originating thread flow depth remains until local pop");
    expectTrue(fuse::profiler::eventCount() == 2u,
               "cross-thread flow records begin on main and finish on worker");
}

void testNullTrackCounterGuard() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::sampleCounter(nullptr, 42);
    fuse::profiler::sampleCounterFloat(nullptr, 1.5);
    fuse::profiler::sampleCounterSnapshotAtFrame(nullptr, 7);
    fuse::profiler::sampleCounterFloatSnapshotAtFrame(nullptr, 2.5);

    expectTrue(fuse::profiler::isEmpty(), "null counter tracks record nothing");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null counter tracks leave chrome export empty");
}

void testDisabledProfilerPreservesActiveDepths() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    {
        FUSE_PROFILE_SCOPE("ignored_scope");
        const fuse::u32 flowId = fuse::profiler::nextFlowId();
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_flow", flowId);
        FUSE_PROFILE_COUNTER("ignored_counter", 1);
        FUSE_PROFILE_ASYNC_FLOW_END("ignored_flow", flowId);
    }

    expectTrue(fuse::profiler::activeNestingDepth() == 0u,
               "disabled profiler does not mutate active scope depth");
    expectTrue(fuse::profiler::activeFlowNestingDepth() == 0u,
               "disabled profiler does not mutate active flow depth");
    expectTrue(fuse::profiler::isEmpty(), "disabled profiler leaves buffer empty");
}

void testCounterScopeAndFlowDepthCombined() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("combo_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("combo_flow", flowId);
        FUSE_PROFILE_COUNTER("combo_budget", 99);
        FUSE_PROFILE_ASYNC_FLOW_END("combo_flow", flowId);
    }

    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(2);
    expectTrue(counter.nestingDepth == 1u, "combo counter inherits scope depth");
    expectTrue(counter.flowNestingDepth == 1u, "combo counter inherits flow depth");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":99,\"depth\":1,\"flow_depth\":1}") != std::string::npos,
               "combo counter export includes scope and flow depth");
}

void testVerifyMacro() {
    resetState();
    fuse::assertion::setSuppressAbortForTests(true);

    bool handlerCalled = false;
    fuse::assertion::setFatalHandler(
        [](const fuse::assertion::FatalContext&, void* userData) {
            *static_cast<bool*>(userData) = true;
        },
        &handlerCalled);

    FUSE_VERIFY(1 + 1 == 2, "math still works");
    expectTrue(!handlerCalled, "verify does not fire on true condition");

    FUSE_VERIFY(false, "expected failure");
    expectTrue(handlerCalled, "verify fires fatal handler on false condition");
}

} // namespace

int main() {
    testProfileScopeRecordsEvents();
    testProfilerDisabledSkipsEvents();
    testFrameBoundaryIncrementsIndex();
    testNestedScopeOrdering();
    testThreadIdStubs();
    testChromeTraceExport();
    testAsyncFlowStubs();
    testAsyncFlowCrossThread();
    testCounterSamples();
    testFloatCounterSamples();
    testAsyncFlowMatchingIdsInExport();
    testChromeTraceNestingDepthExport();
    testChromeTraceEmptyExport();
    testNestedAsyncFlowWithinScope();
    testDisabledProfilerSkipsAsyncFlowAndCounter();
    testChromeTraceEscapedNameExport();
    testChromeTraceEscapedControlCharsExport();
    testChromeTraceEscapedScopeAndFlowNames();
    testCounterSampleInsideScopeDepthExport();
    testMultipleAsyncFlowsInNestedScopes();
    testChromeTraceExportFrameIndex();
    testChromeTraceExportMixedEvents();
    testHasEventsAndEmptyBufferGuards();
    testNestedAsyncFlowDepth();
    testCounterSnapshotAtFrame();
    testCounterSnapshotAtFrameInsideScope();
    testChromeTraceEscapedLowControlChars();
    testNestedFlowInsideScopeExportsBothDepths();
    testDisabledProfilerDoesNotMutateFlowNestingDepth();
    testCounterInsideNestedFlowRecordsFlowDepth();
    testSnapshotAtFrameCounterInsideNestedFlowAndScope();
    testIsEmptyAndIsValidEventIndex();
    testEmptyEventSentinel();
    testActiveDepthQueries();
    testCrossThreadFlowDepthIsThreadLocal();
    testNullTrackCounterGuard();
    testDisabledProfilerPreservesActiveDepths();
    testCounterScopeAndFlowDepthCombined();
    testFatalHandlerHook();
    testVerifyMacro();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_profiler_assert: all tests passed\n");
    return EXIT_SUCCESS;
}
