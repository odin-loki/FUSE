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
    expectTrue(!fuse::profiler::isEventIndexValid(0u), "isEventIndexValid false on empty buffer");
    expectTrue(!fuse::profiler::isEventIndexValid(99u), "isEventIndexValid false when out of range");

    const fuse::profiler::ProfileEvent& emptyEvent = fuse::profiler::eventAt(0);
    expectTrue(emptyEvent.name == nullptr, "eventAt on empty buffer returns sentinel with null name");
    expectTrue(!fuse::profiler::isValidProfileEvent(emptyEvent),
               "isValidProfileEvent false for empty-buffer sentinel");
    expectTrue(emptyEvent.phase == fuse::profiler::EventPhase::Begin,
               "eventAt sentinel keeps default begin phase");

    const fuse::profiler::ProfileEvent& oobEvent = fuse::profiler::eventAt(99);
    expectTrue(oobEvent.name == nullptr, "eventAt out-of-range returns sentinel with null name");
    expectTrue(!fuse::profiler::isValidProfileEvent(oobEvent),
               "isValidProfileEvent false for out-of-range sentinel");

    {
        FUSE_PROFILE_SCOPE("guard_scope");
    }

    expectTrue(fuse::profiler::hasEvents(), "hasEvents true after recording scope");
    expectTrue(fuse::profiler::isEventIndexValid(0u), "isEventIndexValid true for first event");
    expectTrue(fuse::profiler::isEventIndexValid(1u), "isEventIndexValid true for last event");
    expectTrue(!fuse::profiler::isEventIndexValid(2u), "isEventIndexValid false past event count");
    expectTrue(fuse::profiler::isValidProfileEvent(fuse::profiler::eventAt(0)),
               "isValidProfileEvent true for recorded event");
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

void testFlowNestingDepthIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset leaves flow nesting depth at zero");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("depth_outer", outerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "flow begin increments introspection depth");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("depth_inner", innerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 2u, "nested flow begin increments introspection depth");
    FUSE_PROFILE_ASYNC_FLOW_END("depth_inner", innerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "inner flow end restores introspection depth");
    FUSE_PROFILE_ASYNC_FLOW_END("depth_outer", outerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "outer flow end clears introspection depth");
}

void testOrphanAsyncFlowEndIsIgnored() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "orphan guard starts with zero flow depth");
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_finish", 99u);

    expectTrue(fuse::profiler::eventCount() == 0u, "orphan flow finish records no event");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "orphan flow finish does not underflow depth");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 0u, "orphan flow finish does not bump max depth");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("paired_flow", flowId);
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow still records after orphan guard");
}

void testNullNameFlowAndCounterGuards() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginAsyncFlow(nullptr, 1u);
    fuse::profiler::endAsyncFlow(nullptr, 1u);
    fuse::profiler::sampleCounter(nullptr, 42);
    fuse::profiler::sampleCounterFloat(nullptr, 1.5);
    fuse::profiler::sampleCounterSnapshotAtFrame(nullptr, 7);
    fuse::profiler::sampleCounterFloatSnapshotAtFrame(nullptr, 0.25);

    expectTrue(fuse::profiler::eventCount() == 0u, "null flow/counter names record nothing");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "null flow names do not mutate flow depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null-name guard leaves export empty");
}

void testOpenAsyncFlowCountTracking() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset leaves open async flow count at zero");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 2u, "nested flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "inner flow end decrements open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "outer flow end clears open count");
}

void testLastEventIndexAndLastEvent() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::lastEventIndex() == fuse::profiler::kInvalidEventIndex,
               "lastEventIndex is invalid on empty buffer");
    expectTrue(!fuse::profiler::isValidProfileEvent(fuse::profiler::lastEvent()),
               "lastEvent returns sentinel on empty buffer");

    {
        FUSE_PROFILE_SCOPE("first_scope");
    }
    expectTrue(fuse::profiler::lastEventIndex() == 1u, "lastEventIndex points at scope end");
    expectTrue(fuse::profiler::isValidProfileEvent(fuse::profiler::lastEvent()),
               "lastEvent is valid after recording");
    expectTrue(fuse::profiler::lastEvent().phase == fuse::profiler::EventPhase::End,
               "lastEvent returns most recent end phase");
    expectTrue(std::string(fuse::profiler::lastEvent().name) == "first_scope",
               "lastEvent preserves most recent scope name");
}

void testBufferEmptyAndFullGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::isBufferEmpty(), "reset leaves buffer empty");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(!fuse::profiler::hasEvents(), "isBufferEmpty mirrors hasEvents on reset");

    {
        FUSE_PROFILE_SCOPE("single_scope");
    }

    expectTrue(!fuse::profiler::isBufferEmpty(), "recorded events clear isBufferEmpty");
    expectTrue(fuse::profiler::hasEvents(), "hasEvents true after recording");
    expectTrue(!fuse::profiler::isBufferFull(), "two events do not saturate ring buffer");
}

void testDisabledProfilerDoesNotMutateScopeNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    {
        FUSE_PROFILE_SCOPE("ignored_outer");
        {
            FUSE_PROFILE_SCOPE("ignored_inner");
        }
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested scope events");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u,
               "disabled profiler does not mutate scope nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled profiler does not bump max scope nesting depth");

    fuse::profiler::setEnabled(true);
    {
        FUSE_PROFILE_SCOPE("enabled_scope");
    }
    expectTrue(fuse::profiler::maxNestingDepth() == 1u,
               "scope nesting depth resumes cleanly after re-enable");
}

void testDisabledAsyncFlowBeginSkipsDepthAndOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_begin", flowId);

    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "disabled flow begin does not mutate flow nesting depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "disabled flow begin does not increment open async flow count");
    expectTrue(fuse::profiler::eventCount() == 0u, "disabled flow begin records no event");
}

void testNullScopeNameGuard() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope nullScope(nullptr);
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "null scope name records nothing");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "null scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null scope name leaves export empty");
}

void testResetClearsOpenAsyncFlowCount() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open flow count tracks begin");

    fuse::profiler::reset();
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset clears open async flow count");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset clears event buffer");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset clears flow nesting depth");
}

void testDisabledCounterPreservesFlowNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("budget_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "flow begin establishes depth before disable");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_COUNTER("ignored_in_flow", 11);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_snapshot", 22);
    fuse::profiler::setEnabled(true);

    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled counter samples do not mutate flow nesting depth");
    expectTrue(fuse::profiler::eventCount() == 1u, "only flow begin recorded before counter disable");

    FUSE_PROFILE_ASYNC_FLOW_END("budget_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "flow end restores depth after re-enable");
    expectTrue(fuse::profiler::eventCount() == 2u, "flow finish recorded after re-enable");
}

void testScopeNestingDepthIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::nestingDepth() == 0u, "reset leaves scope nesting depth at zero");

    {
        FUSE_PROFILE_SCOPE("depth_outer");
        expectTrue(fuse::profiler::nestingDepth() == 1u, "outer scope increments introspection depth");
        {
            FUSE_PROFILE_SCOPE("depth_inner");
            expectTrue(fuse::profiler::nestingDepth() == 2u, "inner scope increments introspection depth");
        }
        expectTrue(fuse::profiler::nestingDepth() == 1u, "inner scope end restores introspection depth");
    }
    expectTrue(fuse::profiler::nestingDepth() == 0u, "outer scope end clears introspection depth");
}

void testOpenAsyncFlowCountIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset leaves open async flow count at zero");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 2u, "nested flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "inner flow end decrements open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "outer flow end clears open count");
}

void testNullNameProfileScopeGuard() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope nullScope(nullptr);
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "null scope name records nothing");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "null scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u, "null scope name does not bump max nesting depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null scope name leaves export empty");
}

void testResetClearsNestingAndFlowGuardState() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("reset_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_flow", flowId);
        FUSE_PROFILE_COUNTER("reset_counter", 5);
    }

    expectTrue(fuse::profiler::hasEvents(), "pre-reset profiler captured events");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "pre-reset open flow count reflects unmatched begin");

    fuse::profiler::reset();

    expectTrue(!fuse::profiler::hasEvents(), "reset clears event buffer");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "reset clears scope nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset clears flow nesting depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset clears open async flow count");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u, "reset clears max nesting depth");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 0u, "reset clears max flow nesting depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "reset leaves chrome export empty");
}

void testDisabledScopeDoesNotMutateNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    {
        FUSE_PROFILE_SCOPE("ignored_outer");
        {
            FUSE_PROFILE_SCOPE("ignored_inner");
        }
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested scope events");
    expectTrue(fuse::profiler::nestingDepth() == 0u,
               "disabled profiler does not mutate scope nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled profiler does not bump max nesting depth");

    fuse::profiler::setEnabled(true);
    {
        FUSE_PROFILE_SCOPE("enabled_scope");
    }
    expectTrue(fuse::profiler::nestingDepth() == 0u, "scope nesting depth resumes cleanly after re-enable");
    expectTrue(fuse::profiler::maxNestingDepth() == 1u,
               "scope nesting depth resumes recording after re-enable");
}

void testMultipleOrphanAsyncFlowEndsAreIgnored() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_ASYNC_FLOW_END("orphan_a", 1u);
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_b", 2u);
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_c", 3u);

    expectTrue(fuse::profiler::eventCount() == 0u, "multiple orphan flow finishes record nothing");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "orphan finishes do not underflow open count");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "orphan finishes do not underflow flow depth");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_after_orphans", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("paired_after_orphans", flowId);
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow still records after orphan guards");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "paired flow clears open count");
}

void testDisabledBeginAsyncFlowDoesNotIncrementOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_begin", flowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_nested", flowId + 1u);

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled flow begin records nothing");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "disabled flow begin does not increment open count");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "disabled flow begin does not mutate flow nesting depth");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("enabled_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "open count resumes cleanly after re-enable");
    FUSE_PROFILE_ASYNC_FLOW_END("enabled_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "enabled flow pair clears open count");
}

void testEmptyStringNameGuards() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope emptyScope("");
    }
    fuse::profiler::beginAsyncFlow("", 1u);
    fuse::profiler::endAsyncFlow("", 1u);
    fuse::profiler::sampleCounter("", 42);
    fuse::profiler::sampleCounterFloat("", 1.5);
    fuse::profiler::sampleCounterSnapshotAtFrame("", 7);
    fuse::profiler::sampleCounterFloatSnapshotAtFrame("", 0.25);

    expectTrue(fuse::profiler::eventCount() == 0u, "empty-string names record nothing");
    expectTrue(fuse::profiler::isScopeNestingBalanced(), "empty scope name does not unbalance nesting");
    expectTrue(fuse::profiler::isFlowNestingBalanced(), "empty flow names do not unbalance flow depth");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "empty flow names do not leave open async flows");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "empty-string guard leaves export empty");
}

void testNestingBalanceIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::isScopeNestingBalanced(), "reset leaves scope nesting balanced");
    expectTrue(fuse::profiler::isFlowNestingBalanced(), "reset leaves flow nesting balanced");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset leaves no open async flows");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("balance_outer");
        expectTrue(!fuse::profiler::isScopeNestingBalanced(), "active scope reports unbalanced nesting");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("balance_flow", flowId);
        expectTrue(!fuse::profiler::isFlowNestingBalanced(), "open flow reports unbalanced flow nesting");
        expectTrue(fuse::profiler::hasOpenAsyncFlows(), "open flow reports hasOpenAsyncFlows");
        FUSE_PROFILE_ASYNC_FLOW_END("balance_flow", flowId);
        expectTrue(fuse::profiler::isFlowNestingBalanced(), "flow end restores balanced flow nesting");
        expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "flow end clears hasOpenAsyncFlows");
    }
    expectTrue(fuse::profiler::isScopeNestingBalanced(), "scope end restores balanced nesting");
}

void testTryEventAtGuard() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryEventAt clears output on empty buffer");
    expectTrue(!fuse::profiler::isValidProfileEvent(outEvent),
               "tryEventAt output is invalid on empty buffer");

    {
        FUSE_PROFILE_SCOPE("try_scope");
    }

    expectTrue(fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt true for first event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "try_scope",
               "tryEventAt copies event name");

    expectTrue(fuse::profiler::tryEventAt(1u, outEvent), "tryEventAt true for last event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryEventAt copies end phase");

    expectTrue(!fuse::profiler::tryEventAt(2u, outEvent), "tryEventAt false past event count");
    expectTrue(outEvent.name == nullptr, "tryEventAt clears output when out of range");
}

void testResetRestoresNestingBalance() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("reset_balance_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_balance_flow", flowId);
    }

    expectTrue(fuse::profiler::isScopeNestingBalanced(),
               "ended scope restores nesting balance even with open flow");
    expectTrue(!fuse::profiler::isFlowNestingBalanced(), "unmatched flow leaves flow nesting unbalanced");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "unmatched flow leaves open async flows");

    fuse::profiler::reset();

    expectTrue(fuse::profiler::isScopeNestingBalanced(), "reset restores scope nesting balance");
    expectTrue(fuse::profiler::isFlowNestingBalanced(), "reset restores flow nesting balance");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset clears open async flows");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset clears buffer after unmatched nesting");
}

void testIsValidEventNameGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isValidEventName(nullptr), "null name is invalid");
    expectTrue(!fuse::profiler::isValidEventName(""), "empty string name is invalid");
    expectTrue(fuse::profiler::isValidEventName("scope"), "non-empty name is valid");
    expectTrue(!fuse::profiler::isValidProfileEvent(fuse::profiler::emptyProfileEvent()),
               "emptyProfileEvent fails isValidProfileEvent");
}

void testRingCapacityAndEmptyProfileEventSentinel() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::ringCapacity() == 4096u, "ring capacity exposes compile-time buffer size");
    expectTrue(fuse::profiler::emptyProfileEvent().name == nullptr,
               "emptyProfileEvent keeps null name sentinel");

    const fuse::profiler::ProfileEvent& emptyAt = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& oobAt = fuse::profiler::eventAt(99);
    const fuse::profiler::ProfileEvent& lastOnEmpty = fuse::profiler::lastEvent();
    expectTrue(&emptyAt == &fuse::profiler::emptyProfileEvent(),
               "eventAt on empty buffer returns emptyProfileEvent sentinel");
    expectTrue(&oobAt == &fuse::profiler::emptyProfileEvent(),
               "eventAt out-of-range returns emptyProfileEvent sentinel");
    expectTrue(&lastOnEmpty == &fuse::profiler::emptyProfileEvent(),
               "lastEvent on empty buffer returns emptyProfileEvent sentinel");
}

void testTryLastEventGuard() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryLastEvent(outEvent), "tryLastEvent false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryLastEvent clears output on empty buffer");

    {
        FUSE_PROFILE_SCOPE("try_last_scope");
    }

    expectTrue(fuse::profiler::tryLastEvent(outEvent), "tryLastEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastEvent copies last end phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "try_last_scope",
               "tryLastEvent copies last scope name");
}

void testChromeTraceExportPreflightEmptyBuffer() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.canExport(), "preflight allows export when profiler is enabled");
    expectTrue(preflight.bufferEmpty, "preflight marks empty buffer");
    expectTrue(preflight.eventCount == 0u, "preflight event count is zero on reset");
    expectTrue(preflight.exportableEventCount == 0u, "preflight exportable count is zero on reset");
    expectTrue(!preflight.hasExportableEvents(), "preflight hasExportableEvents false on empty buffer");
    expectTrue(preflight.frameIndex == 0u, "preflight frame index matches reset state");
    expectTrue(preflight.openAsyncFlowCount == 0u, "preflight open flow count is zero on reset");
    expectTrue(!preflight.hasOpenAsyncFlows, "preflight hasOpenAsyncFlows false on reset");
    expectTrue(preflight.scopeNestingUnbalanced == false, "preflight scope nesting balanced on reset");
    expectTrue(preflight.flowNestingUnbalanced == false, "preflight flow nesting balanced on reset");
    expectTrue(!preflight.profilerDisabled, "preflight profiler enabled on reset");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"traceEvents\":[]") != std::string::npos,
               "preflight empty buffer still exports valid empty trace");
}

void testChromeTraceExportPreflightWithEvents() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("preflight_scope");
        FUSE_PROFILE_COUNTER("preflight_counter", 9);
    }

    const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.canExport(), "preflight allows export with recorded events");
    expectTrue(!preflight.bufferEmpty, "preflight marks non-empty buffer");
    expectTrue(preflight.eventCount == 3u, "preflight counts scope begin/end and counter");
    expectTrue(preflight.exportableEventCount == 3u, "preflight counts exportable events");
    expectTrue(preflight.hasExportableEvents(), "preflight hasExportableEvents true with trace data");
    expectTrue(preflight.frameIndex == 1u, "preflight frame index reflects beginFrame");
    expectTrue(preflight.scopeNestingUnbalanced == false, "preflight scope nesting balanced after scope end");
    expectTrue(preflight.flowNestingUnbalanced == false, "preflight flow nesting balanced with no open flows");
}

void testChromeTraceExportPreflightOpenFlows() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_flow", flowId);

    const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.canExport(), "preflight allows export with open async flow");
    expectTrue(preflight.hasOpenAsyncFlows, "preflight marks open async flows");
    expectTrue(preflight.openAsyncFlowCount == 1u, "preflight open flow count tracks begin");
    expectTrue(preflight.flowNestingUnbalanced, "preflight flow nesting unbalanced with open flow");
    expectTrue(preflight.exportableEventCount == 1u, "preflight counts open flow start event");

    FUSE_PROFILE_ASYNC_FLOW_END("preflight_flow", flowId);

    const fuse::profiler::ChromeTraceExportPreflight closedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!closedPreflight.hasOpenAsyncFlows, "preflight clears open flow flag after end");
    expectTrue(!closedPreflight.flowNestingUnbalanced, "preflight flow nesting balanced after end");
    expectTrue(closedPreflight.exportableEventCount == 2u, "preflight counts paired flow events");
}

void testChromeTraceExportPreflightDisabledProfiler() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!preflight.canExport(), "preflight blocks export when profiler disabled");
    expectTrue(preflight.profilerDisabled, "preflight marks profiler disabled");
    expectTrue(preflight.bufferEmpty, "preflight buffer empty when disabled");
    expectTrue(!preflight.hasExportableEvents(), "preflight hasExportableEvents false when disabled");
}

void testCrossThreadFlowPreservesOpenCountGuard() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = 55u;
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_guard", flowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "begin on main leaves open flow");

    std::atomic<bool> workerDone{false};
    std::atomic<fuse::u32> workerTid{0};
    std::thread worker([&]() {
        workerTid.store(fuse::platform::chromeTraceThreadId(), std::memory_order_release);
        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_guard", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(fuse::profiler::eventCount() == 2u, "cross-thread end still records finish event");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "cross-thread end clears open flow count");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "begin-thread flow depth remains until same-thread end or reset");

    const fuse::profiler::ProfileEvent& flowFinish = fuse::profiler::eventAt(1);
    expectTrue(flowFinish.threadId == workerTid.load(std::memory_order_acquire),
               "cross-thread finish records worker tid");
}

void testExportableEventCountGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::exportableEventCount() == 0u,
               "exportableEventCount is zero on empty buffer");

    {
        FUSE_PROFILE_SCOPE("exportable_scope");
        FUSE_PROFILE_COUNTER("exportable_counter", 4);
    }

    expectTrue(fuse::profiler::exportableEventCount() == 3u,
               "exportableEventCount counts scope begin/end and counter");
    expectTrue(fuse::profiler::exportableEventCount() == fuse::profiler::eventCount(),
               "exportableEventCount matches eventCount for valid names");

    fuse::profiler::beginAsyncFlow(nullptr, 1u);
    expectTrue(fuse::profiler::exportableEventCount() == 3u,
               "null-name flow attempt does not affect exportableEventCount");
}

void testIsEventExportableGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isEventExportable(0u), "isEventExportable false on empty buffer");
    expectTrue(!fuse::profiler::isEventExportable(99u), "isEventExportable false when out of range");

    {
        FUSE_PROFILE_SCOPE("exportable_event");
    }

    expectTrue(fuse::profiler::isEventExportable(0u), "isEventExportable true for begin event");
    expectTrue(fuse::profiler::isEventExportable(1u), "isEventExportable true for end event");
    expectTrue(!fuse::profiler::isEventExportable(2u), "isEventExportable false past event count");
}

void testFirstEventIndexAndTryFirstEventGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::firstEventIndex() == fuse::profiler::kInvalidEventIndex,
               "firstEventIndex invalid on empty buffer");

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryFirstEvent(outEvent), "tryFirstEvent false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryFirstEvent clears output on empty buffer");

    {
        FUSE_PROFILE_SCOPE("first_event_scope");
    }

    expectTrue(fuse::profiler::firstEventIndex() == 0u, "firstEventIndex is zero after recording");
    expectTrue(fuse::profiler::tryFirstEvent(outEvent), "tryFirstEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryFirstEvent copies begin phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "first_event_scope",
               "tryFirstEvent copies first scope name");
}

void testHasUnbalancedNestingGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::hasUnbalancedNesting(), "reset leaves nesting balanced");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("balance_probe");
        expectTrue(fuse::profiler::hasUnbalancedNesting(), "active scope reports unbalanced nesting");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("balance_flow", flowId);
        expectTrue(fuse::profiler::hasUnbalancedNesting(), "open flow inside scope stays unbalanced");
        FUSE_PROFILE_ASYNC_FLOW_END("balance_flow", flowId);
        expectTrue(fuse::profiler::hasUnbalancedNesting(), "active scope still unbalanced after flow end");
    }
    expectTrue(!fuse::profiler::hasUnbalancedNesting(), "ended scope restores balanced nesting");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("unmatched_flow", flowId);
    expectTrue(fuse::profiler::hasUnbalancedNesting(), "unmatched flow reports unbalanced nesting");
}

void testIsFlowDepthDetachedGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isFlowDepthDetached(), "reset leaves flow depth attached");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("attached_flow", flowId);
    expectTrue(!fuse::profiler::isFlowDepthDetached(), "same-thread flow keeps depth attached");
    FUSE_PROFILE_ASYNC_FLOW_END("attached_flow", flowId);
    expectTrue(!fuse::profiler::isFlowDepthDetached(), "paired flow end clears detached state");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_detach", flowId);
    expectTrue(!fuse::profiler::isFlowDepthDetached(), "begin on main keeps depth attached");

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_detach", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(fuse::profiler::isFlowDepthDetached(),
               "cross-thread finish leaves begin-thread flow depth detached");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "begin-thread flow depth remains until same-thread cleanup or reset");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "cross-thread finish clears global open async flow count");
}

void testMixedEmptyAndValidNameGuards() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope emptyScope("");
        fuse::profiler::ProfileScope nullScope(nullptr);
    }
    fuse::profiler::beginAsyncFlow("", 1u);
    fuse::profiler::sampleCounter("", 1);
    fuse::profiler::endAsyncFlow("", 1u);

    expectTrue(fuse::profiler::eventCount() == 0u, "empty/null names record nothing before valid event");

    {
        FUSE_PROFILE_SCOPE("valid_after_empty");
        FUSE_PROFILE_COUNTER("valid_counter", 2);
    }

    expectTrue(fuse::profiler::eventCount() == 3u, "valid events record after empty-name attempts");
    expectTrue(fuse::profiler::exportableEventCount() == 3u,
               "exportable count matches after mixed empty/valid attempts");
    expectTrue(fuse::profiler::isScopeNestingBalanced(), "valid scope nesting balanced after empty attempts");
    expectTrue(fuse::profiler::isFlowNestingBalanced(), "valid flow nesting balanced after empty attempts");
}

void testDisabledEndAsyncFlowPreservesOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("disable_guard_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open count");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_ASYNC_FLOW_END("disable_guard_flow", flowId);

    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "disabled flow end does not decrement open count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "disabled flow end leaves open async flows");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled flow end does not mutate thread-local flow depth");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_END("disable_guard_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "enabled flow end clears open count after disable guard");
}

void testChromeTraceExportPreflightActiveScope() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("preflight_active_scope");
        const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
        expectTrue(preflight.canExport(), "preflight allows export inside active scope");
        expectTrue(preflight.scopeNestingUnbalanced, "preflight marks active scope as unbalanced");
        expectTrue(preflight.hasUnbalancedNesting(), "preflight hasUnbalancedNesting inside active scope");
        expectTrue(preflight.activeScopeNestingDepth == 1u, "preflight reports active scope depth");
        expectTrue(preflight.maxScopeNestingDepth == 1u, "preflight reports max scope depth");
        expectTrue(preflight.exportableEventCount == 1u, "preflight counts begin event inside active scope");
        expectTrue(preflight.eventCount == 1u, "preflight event count includes active begin");
    }

    const fuse::profiler::ChromeTraceExportPreflight closedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!closedPreflight.scopeNestingUnbalanced, "preflight scope balanced after scope end");
    expectTrue(!closedPreflight.hasUnbalancedNesting(), "preflight nesting balanced after scope end");
    expectTrue(closedPreflight.exportableEventCount == 2u, "preflight counts paired scope events");
}

void testChromeTraceExportPreflightNestingDepths() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("preflight_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_outer_flow", outerFlowId);
        {
            FUSE_PROFILE_SCOPE("preflight_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_inner_flow", innerFlowId);
            const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
            expectTrue(preflight.activeScopeNestingDepth == 2u, "preflight reports inner scope depth");
            expectTrue(preflight.activeFlowNestingDepth == 2u, "preflight reports nested flow depth");
            expectTrue(preflight.maxScopeNestingDepth == 2u, "preflight max scope depth tracks inner scope");
            expectTrue(preflight.maxFlowNestingDepth == 2u, "preflight max flow depth tracks inner flow");
            expectTrue(preflight.openAsyncFlowCount == 2u, "preflight open flow count tracks nested begins");
            expectTrue(preflight.hasUnbalancedNesting(), "preflight nesting unbalanced with open scopes/flows");
            FUSE_PROFILE_ASYNC_FLOW_END("preflight_inner_flow", innerFlowId);
        }
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_outer_flow", outerFlowId);
    }

    const fuse::profiler::ChromeTraceExportPreflight closedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(closedPreflight.activeScopeNestingDepth == 0u, "preflight active scope depth clears after end");
    expectTrue(closedPreflight.activeFlowNestingDepth == 0u, "preflight active flow depth clears after end");
    expectTrue(!closedPreflight.hasUnbalancedNesting(), "preflight nesting balanced after nested teardown");
}

void testChromeTraceExportPreflightDetachedFlow() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_detach", flowId);

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_detach", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.flowDepthDetached, "preflight marks detached flow depth after cross-thread end");
    expectTrue(!preflight.hasOpenAsyncFlows, "preflight has no open flows after cross-thread end");
    expectTrue(preflight.flowNestingUnbalanced, "preflight flow nesting unbalanced on begin thread");
    expectTrue(preflight.activeFlowNestingDepth == 1u, "preflight active flow depth remains on begin thread");
    expectTrue(preflight.openAsyncFlowCount == 0u, "preflight open flow count cleared by worker end");
}

void testIsProfileEventSentinelGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::emptyProfileEvent()),
               "emptyProfileEvent is a sentinel");
    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::eventAt(0)),
               "eventAt on empty buffer returns sentinel shape");
    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::lastEvent()),
               "lastEvent on empty buffer returns sentinel shape");

    fuse::profiler::ProfileEvent cleared{};
    expectTrue(fuse::profiler::isProfileEventSentinel(cleared),
               "default-constructed event matches sentinel shape");

    {
        FUSE_PROFILE_SCOPE("sentinel_scope");
    }

    expectTrue(!fuse::profiler::isProfileEventSentinel(fuse::profiler::eventAt(0)),
               "recorded begin event is not a sentinel");
    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::eventAt(99)),
               "out-of-range eventAt still returns sentinel shape");
}

void testInvalidNameEventCountGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::invalidNameEventCount() == 0u,
               "invalidNameEventCount is zero on empty buffer");
    expectTrue(!fuse::profiler::hasInvalidNameEvents(), "hasInvalidNameEvents false on empty buffer");

    {
        FUSE_PROFILE_SCOPE("valid_scope");
        FUSE_PROFILE_COUNTER("valid_counter", 3);
    }

    expectTrue(fuse::profiler::invalidNameEventCount() == 0u,
               "valid events keep invalidNameEventCount at zero");
    expectTrue(!fuse::profiler::hasInvalidNameEvents(), "hasInvalidNameEvents false for valid trace");
    expectTrue(fuse::profiler::invalidNameEventCount() + fuse::profiler::exportableEventCount()
                   == fuse::profiler::eventCount(),
               "invalid + exportable counts reconcile with eventCount");
}

void testTryExportableEventAtGuard() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryExportableEventAt(0u, outEvent),
               "tryExportableEventAt false on empty buffer");
    expectTrue(fuse::profiler::isProfileEventSentinel(outEvent),
               "tryExportableEventAt clears output on empty buffer");

    {
        FUSE_PROFILE_SCOPE("exportable_scope");
    }

    expectTrue(fuse::profiler::tryExportableEventAt(0u, outEvent),
               "tryExportableEventAt true for begin event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryExportableEventAt copies begin phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "exportable_scope",
               "tryExportableEventAt copies scope name");

    expectTrue(fuse::profiler::tryExportableEventAt(1u, outEvent),
               "tryExportableEventAt true for end event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryExportableEventAt copies end phase");

    expectTrue(!fuse::profiler::tryExportableEventAt(2u, outEvent),
               "tryExportableEventAt false past event count");
    expectTrue(fuse::profiler::isProfileEventSentinel(outEvent),
               "tryExportableEventAt clears output when out of range");
}

void testFindEventIndexByPhaseGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::Begin)
                   == fuse::profiler::kInvalidEventIndex,
               "findFirstEventIndexByPhase invalid on empty buffer");
    expectTrue(fuse::profiler::findLastEventIndexByPhase(fuse::profiler::EventPhase::End)
                   == fuse::profiler::kInvalidEventIndex,
               "findLastEventIndexByPhase invalid on empty buffer");
    expectTrue(fuse::profiler::countEventsByPhase(fuse::profiler::EventPhase::Counter) == 0u,
               "countEventsByPhase is zero on empty buffer");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("phase_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("phase_flow", flowId);
        FUSE_PROFILE_COUNTER("phase_counter", 5);
        FUSE_PROFILE_ASYNC_FLOW_END("phase_flow", flowId);
    }

    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::Begin) == 0u,
               "findFirstEventIndexByPhase locates scope begin");
    expectTrue(fuse::profiler::findLastEventIndexByPhase(fuse::profiler::EventPhase::End) == 4u,
               "findLastEventIndexByPhase locates scope end");
    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::FlowStart) == 1u,
               "findFirstEventIndexByPhase locates flow start");
    expectTrue(fuse::profiler::findLastEventIndexByPhase(fuse::profiler::EventPhase::Counter) == 2u,
               "findLastEventIndexByPhase locates counter sample");
    expectTrue(fuse::profiler::countEventsByPhase(fuse::profiler::EventPhase::Begin) == 1u,
               "countEventsByPhase counts scope begins");
    expectTrue(fuse::profiler::countEventsByPhase(fuse::profiler::EventPhase::Counter) == 1u,
               "countEventsByPhase counts counter samples");
    expectTrue(fuse::profiler::countEventsByPhase(fuse::profiler::EventPhase::FlowFinish) == 1u,
               "countEventsByPhase counts flow finishes");
}

void testIsCrossThreadFlowHandoffPendingGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isCrossThreadFlowHandoffPending(),
               "reset leaves cross-thread handoff pending false");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("handoff_flow", flowId);
    expectTrue(!fuse::profiler::isCrossThreadFlowHandoffPending(),
               "same-thread open flow is not a pending handoff");

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
        FUSE_PROFILE_ASYNC_FLOW_END("handoff_flow", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(fuse::profiler::isFlowDepthDetached(),
               "cross-thread finish leaves flow depth detached");
    expectTrue(fuse::profiler::isCrossThreadFlowHandoffPending(),
               "cross-thread finish reports pending handoff on begin thread");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "cross-thread finish clears global open flow count");

    fuse::profiler::reset();
    expectTrue(!fuse::profiler::isCrossThreadFlowHandoffPending(),
               "reset clears cross-thread handoff pending state");
}

void testChromeTraceExportPreflightSafetyFlags() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.canExportSafely(), "empty balanced buffer can export safely");
    expectTrue(!emptyPreflight.hasInvalidNameEvents, "empty preflight has no invalid-name events");
    expectTrue(emptyPreflight.invalidNameEventCount == 0u, "empty preflight invalid-name count is zero");
    expectTrue(!emptyPreflight.ringBufferFull, "empty preflight ring buffer is not full");
    expectTrue(!emptyPreflight.crossThreadFlowHandoffPending,
               "empty preflight has no pending cross-thread handoff");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("preflight_safe_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_safe_flow", flowId);
        const fuse::profiler::ChromeTraceExportPreflight activePreflight =
            fuse::profiler::preflightChromeTraceExport();
        expectTrue(activePreflight.canExport(), "preflight allows export with open scope and flow");
        expectTrue(!activePreflight.canExportSafely(),
                   "preflight blocks safe export with unbalanced nesting");
        expectTrue(activePreflight.hasUnbalancedNesting(),
                   "preflight marks active scope/flow as unbalanced");
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_safe_flow", flowId);
    }

    const fuse::profiler::ChromeTraceExportPreflight closedPreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(closedPreflight.canExportSafely(), "balanced trace can export safely");
    expectTrue(closedPreflight.exportableEventCount == closedPreflight.eventCount,
               "safe preflight exportable count matches event count");
}

void testChromeTraceExportPreflightCrossThreadHandoff() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_handoff", flowId);

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_handoff", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.crossThreadFlowHandoffPending,
               "preflight marks pending cross-thread handoff");
    expectTrue(preflight.flowDepthDetached, "preflight flow depth detached after cross-thread end");
    expectTrue(!preflight.canExportSafely(), "preflight blocks safe export during handoff cleanup");
    expectTrue(preflight.canExport(), "preflight still allows raw export during handoff cleanup");
}

void testEmptyNameAttemptsDoNotAffectPhaseLookup() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginAsyncFlow("", 1u);
    fuse::profiler::sampleCounter("", 1);
    fuse::profiler::endAsyncFlow("", 1u);
    {
        fuse::profiler::ProfileScope emptyScope("");
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "empty-name attempts record nothing");
    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::Counter)
                   == fuse::profiler::kInvalidEventIndex,
               "phase lookup stays invalid after empty-name attempts");

    FUSE_PROFILE_COUNTER("valid_after_empty", 4);
    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::Counter) == 0u,
               "phase lookup finds valid counter after empty-name attempts");

    fuse::profiler::ProfileEvent counterEvent{};
    expectTrue(fuse::profiler::tryExportableEventAt(0u, counterEvent),
               "tryExportableEventAt succeeds for valid counter after empty-name attempts");
    expectTrue(counterEvent.phase == fuse::profiler::EventPhase::Counter,
               "exportable lookup copies counter phase after empty-name attempts");
}

void testIsBlankEventNameGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::isBlankEventName(nullptr), "null name is blank");
    expectTrue(fuse::profiler::isBlankEventName(""), "empty string is blank");
    expectTrue(fuse::profiler::isBlankEventName(" \t\r\n"), "whitespace-only name is blank");
    expectTrue(!fuse::profiler::isBlankEventName("scope"), "non-blank name is not blank");
    expectTrue(fuse::profiler::isValidEventName(" \t"), "whitespace-only name still passes isValidEventName");
    expectTrue(fuse::profiler::isBlankEventName(" \t"), "whitespace-only name fails isBlankEventName");

    {
        fuse::profiler::ProfileScope whitespaceScope(" \t");
    }
    expectTrue(fuse::profiler::eventCount() == 2u,
               "whitespace-only scope name still records on valid path");
    expectTrue(fuse::profiler::countEventsByName(" \t") == 2u,
               "name lookup finds whitespace-only scope events");

void testFindEventIndexByNameGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::findFirstEventIndexByName(nullptr) == fuse::profiler::kInvalidEventIndex,
               "findFirstEventIndexByName rejects null name");
    expectTrue(fuse::profiler::findLastEventIndexByName("") == fuse::profiler::kInvalidEventIndex,
               "findLastEventIndexByName rejects empty name");
    expectTrue(fuse::profiler::countEventsByName(nullptr) == 0u,
               "countEventsByName returns zero for null name");
    expectTrue(!fuse::profiler::hasEventsWithName(""), "hasEventsWithName false for empty name");

    {
        FUSE_PROFILE_SCOPE("name_lookup_outer");
        FUSE_PROFILE_COUNTER("name_lookup_counter", 1);
            FUSE_PROFILE_SCOPE("name_lookup_inner");
        }

    expectTrue(fuse::profiler::findFirstEventIndexByName("name_lookup_outer") == 0u,
               "findFirstEventIndexByName locates outer scope begin");
    expectTrue(fuse::profiler::findLastEventIndexByName("name_lookup_outer") == 4u,
               "findLastEventIndexByName locates outer scope end");
    expectTrue(fuse::profiler::findFirstEventIndexByName("name_lookup_inner") == 2u,
               "findFirstEventIndexByName locates inner scope begin");
    expectTrue(fuse::profiler::findLastEventIndexByName("name_lookup_counter") == 1u,
               "findLastEventIndexByName locates counter sample");
    expectTrue(fuse::profiler::countEventsByName("name_lookup_outer") == 2u,
               "countEventsByName counts paired scope events");
    expectTrue(fuse::profiler::countEventsByName("name_lookup_counter") == 1u,
               "countEventsByName counts counter sample");
    expectTrue(fuse::profiler::hasEventsWithName("name_lookup_inner"),
               "hasEventsWithName true for recorded inner scope");
    expectTrue(!fuse::profiler::hasEventsWithName("missing_scope"),
               "hasEventsWithName false for missing name");

void testTryEventByNameGuard() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryFirstEventByName(nullptr, outEvent),
               "tryFirstEventByName false for null name");
    expectTrue(fuse::profiler::isProfileEventSentinel(outEvent),
               "tryFirstEventByName clears output for null name");
    expectTrue(!fuse::profiler::tryLastEventByName("", outEvent),
               "tryLastEventByName false for empty name");

        FUSE_PROFILE_SCOPE("try_name_scope");
        FUSE_PROFILE_COUNTER("try_name_counter", 8);

    expectTrue(fuse::profiler::tryFirstEventByName("try_name_scope", outEvent),
               "tryFirstEventByName true for scope begin");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin,
               "tryFirstEventByName copies begin phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "try_name_scope",
               "tryFirstEventByName copies scope name");

    expectTrue(fuse::profiler::tryLastEventByName("try_name_scope", outEvent),
               "tryLastEventByName true for scope end");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End,
               "tryLastEventByName copies end phase");

    expectTrue(fuse::profiler::tryFirstEventByName("try_name_counter", outEvent),
               "tryFirstEventByName true for counter sample");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Counter,
               "tryFirstEventByName copies counter phase");
    expectTrue(outEvent.counterIntValue == 8, "tryFirstEventByName copies counter value");

    expectTrue(!fuse::profiler::tryFirstEventByName("missing_name", outEvent),
               "tryFirstEventByName false for missing name");
               "tryFirstEventByName clears output for missing name");
               "findFirstEventIndexByName rejects null query");
    expectTrue(fuse::profiler::findFirstEventIndexByName("") == fuse::profiler::kInvalidEventIndex,
               "findFirstEventIndexByName rejects empty query");
    expectTrue(fuse::profiler::findLastEventIndexByName("missing") == fuse::profiler::kInvalidEventIndex,
               "findLastEventIndexByName invalid on empty buffer");
    expectTrue(fuse::profiler::countEventsByName("missing") == 0u,
               "countEventsByName is zero on empty buffer");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
        FUSE_PROFILE_SCOPE("name_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("name_flow", flowId);
        FUSE_PROFILE_COUNTER("name_counter", 3);
        FUSE_PROFILE_ASYNC_FLOW_END("name_flow", flowId);

    expectTrue(fuse::profiler::findFirstEventIndexByName("name_outer") == 0u,
               "findFirstEventIndexByName locates scope begin");
    expectTrue(fuse::profiler::findLastEventIndexByName("name_outer") == 4u,
               "findLastEventIndexByName locates scope end");
    expectTrue(fuse::profiler::findFirstEventIndexByName("name_flow") == 1u,
               "findFirstEventIndexByName locates flow start");
    expectTrue(fuse::profiler::findLastEventIndexByName("name_counter") == 2u,
    expectTrue(fuse::profiler::countEventsByName("name_flow") == 2u,
               "countEventsByName counts paired flow events");
    expectTrue(fuse::profiler::countEventsByName("name_outer") == 2u,
}

void testFindEventIndexByFlowIdGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isValidFlowId(0u), "flow id zero is invalid");
    expectTrue(fuse::profiler::isValidFlowId(1u), "non-zero flow id is valid");
    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(0u) == fuse::profiler::kInvalidEventIndex,
               "findFirstEventIndexByFlowId rejects zero flow id");
    expectTrue(fuse::profiler::countEventsByFlowId(0u) == 0u,
               "countEventsByFlowId returns zero for zero flow id");
    expectTrue(!fuse::profiler::hasEventsWithFlowId(0u), "hasEventsWithFlowId false for zero flow id");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow_lookup_outer", outerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow_lookup_inner", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("flow_lookup_inner", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("flow_lookup_outer", outerFlowId);

    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(outerFlowId) == 0u,
               "findFirstEventIndexByFlowId locates outer flow start");
    expectTrue(fuse::profiler::findLastEventIndexByFlowId(outerFlowId) == 3u,
               "findLastEventIndexByFlowId locates outer flow finish");
    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(innerFlowId) == 1u,
    expectTrue(fuse::profiler::findLastEventIndexByFlowId(99u) == fuse::profiler::kInvalidEventIndex,
               "findLastEventIndexByFlowId invalid on empty buffer");
    expectTrue(fuse::profiler::countEventsByFlowId(99u) == 0u,
               "countEventsByFlowId is zero on empty buffer");

    {
        FUSE_PROFILE_SCOPE("flow_lookup_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow_lookup_outer_io", outerFlowId);
            FUSE_PROFILE_SCOPE("flow_lookup_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow_lookup_inner_io", innerFlowId);
            FUSE_PROFILE_ASYNC_FLOW_END("flow_lookup_inner_io", innerFlowId);
        }
        FUSE_PROFILE_ASYNC_FLOW_END("flow_lookup_outer_io", outerFlowId);

    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(outerFlowId) == 1u,
    expectTrue(fuse::profiler::findLastEventIndexByFlowId(outerFlowId) == 6u,
    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(innerFlowId) == 3u,
               "findFirstEventIndexByFlowId locates inner flow start");
    expectTrue(fuse::profiler::countEventsByFlowId(outerFlowId) == 2u,
               "countEventsByFlowId counts paired outer flow events");
    expectTrue(fuse::profiler::countEventsByFlowId(innerFlowId) == 2u,
               "countEventsByFlowId counts paired inner flow events");
    expectTrue(fuse::profiler::hasEventsWithFlowId(outerFlowId),
               "hasEventsWithFlowId true for recorded outer flow");
    expectTrue(!fuse::profiler::hasEventsWithFlowId(9999u),
               "hasEventsWithFlowId false for missing flow id");

    const fuse::profiler::ProfileEvent& outerStart = fuse::profiler::eventAt(0);
    expectTrue(fuse::profiler::isFlowPhaseEvent(outerStart),
               "flow start event is a flow phase event");
    expectTrue(fuse::profiler::eventMatchesFlowId(outerStart, outerFlowId),
               "eventMatchesFlowId matches outer flow start");
    expectTrue(!fuse::profiler::eventMatchesFlowId(outerStart, innerFlowId),
               "eventMatchesFlowId rejects mismatched flow id");
}

void testTryFlowEventGuard() {
    expectTrue(fuse::profiler::findFirstEventIndexByName("flow_lookup_outer") == 0u,
               "name lookup ignores flow id field on scope events");

void testTryFindEventByNameGuard() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryFirstFlowEvent(0u, outEvent),
               "tryFirstFlowEvent false for zero flow id");
    expectTrue(fuse::profiler::isProfileEventSentinel(outEvent),
               "tryFirstFlowEvent clears output for zero flow id");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("try_flow_event", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("try_flow_event", flowId);

    expectTrue(fuse::profiler::tryFirstFlowEvent(flowId, outEvent),
               "tryFirstFlowEvent true for flow start");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::FlowStart,
               "tryFirstFlowEvent copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFirstFlowEvent copies flow id");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "try_flow_event",
               "tryFirstFlowEvent copies flow name");

    expectTrue(fuse::profiler::tryLastFlowEvent(flowId, outEvent),
               "tryLastFlowEvent true for flow finish");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::FlowFinish,
               "tryLastFlowEvent copies flow finish phase");

    expectTrue(!fuse::profiler::tryFirstFlowEvent(flowId + 1000u, outEvent),
               "tryFirstFlowEvent false for missing flow id");
               "tryFirstFlowEvent clears output for missing flow id");
}

void testEventNameMatchesGuard() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::eventNameMatches(fuse::profiler::emptyProfileEvent(), "scope"),
               "eventNameMatches false for sentinel event");
    expectTrue(!fuse::profiler::eventNameMatches(fuse::profiler::emptyProfileEvent(), nullptr),
               "eventNameMatches false for null lookup name");

    {
        FUSE_PROFILE_SCOPE("match_scope");

    const fuse::profiler::ProfileEvent& begin = fuse::profiler::eventAt(0);
    expectTrue(fuse::profiler::eventNameMatches(begin, "match_scope"),
               "eventNameMatches true for matching scope name");
    expectTrue(!fuse::profiler::eventNameMatches(begin, "other_scope"),
               "eventNameMatches false for mismatched scope name");
    expectTrue(!fuse::profiler::eventNameMatches(begin, ""),
               "eventNameMatches false for empty lookup name");

void testEmptyNameAttemptsDoNotAffectNameAndFlowLookup() {

    fuse::profiler::beginAsyncFlow("", 1u);
    fuse::profiler::sampleCounter("", 1);
    fuse::profiler::endAsyncFlow("", 1u);
        fuse::profiler::ProfileScope emptyScope("");

    expectTrue(fuse::profiler::eventCount() == 0u, "empty-name attempts record nothing");
    expectTrue(fuse::profiler::findFirstEventIndexByName("") == fuse::profiler::kInvalidEventIndex,
               "name lookup stays invalid after empty-name attempts");
    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(1u) == fuse::profiler::kInvalidEventIndex,
               "flow lookup stays invalid after empty-name flow attempts");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("valid_after_empty", flowId);
    FUSE_PROFILE_COUNTER("valid_after_empty_counter", 3);
    FUSE_PROFILE_ASYNC_FLOW_END("valid_after_empty", flowId);

    expectTrue(fuse::profiler::findFirstEventIndexByName("valid_after_empty_counter") == 1u,
               "name lookup finds valid counter after empty-name attempts");
    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(flowId) == 0u,
               "flow lookup finds valid flow after empty-name attempts");

void testPreflightNestingAndAsyncFlowBalanced() {
    expectTrue(!fuse::profiler::tryFindFirstEventByName(nullptr, outEvent),
               "tryFindFirstEventByName false for null query");
               "tryFindFirstEventByName clears output for null query");
    expectTrue(!fuse::profiler::tryFindLastEventByName("missing", outEvent),
               "tryFindLastEventByName false on empty buffer");

        FUSE_PROFILE_SCOPE("named_scope");
        FUSE_PROFILE_COUNTER("named_counter", 8);

    expectTrue(fuse::profiler::tryFindFirstEventByName("named_scope", outEvent),
               "tryFindFirstEventByName true for scope begin");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin,
               "tryFindFirstEventByName copies begin phase");
    expectTrue(fuse::profiler::tryFindLastEventByName("named_counter", outEvent),
               "tryFindLastEventByName true for counter sample");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Counter,
               "tryFindLastEventByName copies counter phase");

void testTryFindFlowEventGuard() {

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryFindFirstFlowEvent(0u, outEvent),
               "tryFindFirstFlowEvent false for zero flow id");
               "tryFindFirstFlowEvent clears output for zero flow id");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow_try_lookup", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("flow_try_lookup", flowId);

    expectTrue(fuse::profiler::tryFindFirstFlowEvent(flowId, outEvent),
               "tryFindFirstFlowEvent true for flow start");
               "tryFindFirstFlowEvent copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFindFirstFlowEvent preserves flow id");

    expectTrue(fuse::profiler::tryFindLastFlowEvent(flowId, outEvent),
               "tryFindLastFlowEvent true for flow finish");
               "tryFindLastFlowEvent copies flow finish phase");

void testTryExportableFirstAndLastEventGuard() {

    expectTrue(!fuse::profiler::tryExportableFirstEvent(outEvent),
               "tryExportableFirstEvent false on empty buffer");
               "tryExportableFirstEvent clears output on empty buffer");
    expectTrue(!fuse::profiler::tryExportableLastEvent(outEvent),
               "tryExportableLastEvent false on empty buffer");

        FUSE_PROFILE_SCOPE("exportable_bookends");

    expectTrue(fuse::profiler::tryExportableFirstEvent(outEvent),
               "tryExportableFirstEvent true after recording");
               "tryExportableFirstEvent copies first begin phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "exportable_bookends",
               "tryExportableFirstEvent copies first scope name");

    expectTrue(fuse::profiler::tryExportableLastEvent(outEvent),
               "tryExportableLastEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End,
               "tryExportableLastEvent copies last end phase");

void testTryFindEventIndexGuards() {

    fuse::u32 outIndex = 0u;
    expectTrue(!fuse::profiler::tryFindFirstEventIndexByPhase(fuse::profiler::EventPhase::Begin, outIndex),
               "tryFindFirstEventIndexByPhase false on empty buffer");
    expectTrue(outIndex == fuse::profiler::kInvalidEventIndex,
               "tryFindFirstEventIndexByPhase clears output on empty buffer");
    expectTrue(!fuse::profiler::tryFindFirstEventIndexByName(nullptr, outIndex),
               "tryFindFirstEventIndexByName false for null query");
    expectTrue(!fuse::profiler::tryFindFirstEventIndexByFlowId(0u, outIndex),
               "tryFindFirstEventIndexByFlowId false for zero flow id");

        FUSE_PROFILE_SCOPE("index_lookup_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("index_lookup_flow", flowId);
        FUSE_PROFILE_COUNTER("index_lookup_counter", 6);
        FUSE_PROFILE_ASYNC_FLOW_END("index_lookup_flow", flowId);

    expectTrue(fuse::profiler::tryFindFirstEventIndexByName("index_lookup_counter", outIndex),
               "tryFindFirstEventIndexByName true for counter");
    expectTrue(outIndex == 2u, "tryFindFirstEventIndexByName returns counter index");
    expectTrue(fuse::profiler::tryFindFirstEventIndexByFlowId(flowId, outIndex),
               "tryFindFirstEventIndexByFlowId true for flow start");
    expectTrue(outIndex == 1u, "tryFindFirstEventIndexByFlowId returns flow start index");
    expectTrue(fuse::profiler::tryFindLastEventIndexByFlowId(flowId, outIndex),
               "tryFindLastEventIndexByFlowId true for flow finish");
    expectTrue(outIndex == 3u, "tryFindLastEventIndexByFlowId returns flow finish index");

void testPreflightProfileScopeGuard() {

    const fuse::profiler::ProfileScopePreflight nullPreflight =
        fuse::profiler::preflightProfileScope(nullptr);
    expectTrue(nullPreflight.invalidName, "scope preflight marks null name invalid");
    expectTrue(!nullPreflight.canEnter, "scope preflight blocks null name");

    const fuse::profiler::ProfileScopePreflight emptyPreflight =
        fuse::profiler::preflightProfileScope("");
    expectTrue(emptyPreflight.invalidName, "scope preflight marks empty name invalid");
    expectTrue(!emptyPreflight.canEnter, "scope preflight blocks empty name");

    const fuse::profiler::ProfileScopePreflight validPreflight =
        fuse::profiler::preflightProfileScope("valid_scope");
    expectTrue(validPreflight.canEnter, "scope preflight allows valid name");
    expectTrue(!validPreflight.invalidName, "scope preflight accepts valid name");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ProfileScopePreflight disabledPreflight =
        fuse::profiler::preflightProfileScope("ignored_scope");
    expectTrue(disabledPreflight.profilerDisabled, "scope preflight marks disabled profiler");
    expectTrue(!disabledPreflight.canEnter, "scope preflight blocks entry when disabled");

void testPreflightAsyncFlowGuards() {


    const fuse::profiler::AsyncFlowBeginPreflight nullBegin =
        fuse::profiler::preflightBeginAsyncFlow(nullptr, flowId);
    expectTrue(nullBegin.invalidName, "flow begin preflight marks null name invalid");
    expectTrue(!nullBegin.canBegin, "flow begin preflight blocks null name");

    const fuse::profiler::AsyncFlowEndPreflight orphanEnd =
        fuse::profiler::preflightEndAsyncFlow("orphan_flow", flowId);
    expectTrue(orphanEnd.wouldUnderflowOpenCount,
               "flow end preflight marks orphan finish as underflow");
    expectTrue(!orphanEnd.canEnd, "flow end preflight blocks orphan finish");

    const fuse::profiler::AsyncFlowBeginPreflight validBegin =
        fuse::profiler::preflightBeginAsyncFlow("paired_flow", flowId);
    expectTrue(validBegin.canBegin, "flow begin preflight allows valid begin");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);

    const fuse::profiler::AsyncFlowEndPreflight validEnd =
        fuse::profiler::preflightEndAsyncFlow("paired_flow", flowId);
    expectTrue(validEnd.canEnd, "flow end preflight allows valid end after begin");
    expectTrue(!validEnd.wouldUnderflowOpenCount,
               "flow end preflight clears underflow after begin");

    const fuse::profiler::AsyncFlowBeginPreflight disabledBegin =
        fuse::profiler::preflightBeginAsyncFlow("ignored_flow", flowId);
    expectTrue(disabledBegin.profilerDisabled, "flow begin preflight marks disabled profiler");
    expectTrue(!disabledBegin.canBegin, "flow begin preflight blocks begin when disabled");

    const fuse::profiler::AsyncFlowEndPreflight disabledEnd =
    expectTrue(disabledEnd.profilerDisabled, "flow end preflight marks disabled profiler");
    expectTrue(!disabledEnd.canEnd, "flow end preflight blocks end when disabled");

void testPreflightNestingAsyncFlowGuard() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::NestingAsyncFlowPreflight resetPreflight =
        fuse::profiler::preflightNestingAndAsyncFlow();
    expectTrue(resetPreflight.scopeNestingBalanced, "reset leaves scope nesting balanced");
    expectTrue(resetPreflight.flowNestingBalanced, "reset leaves flow nesting balanced");
    expectTrue(resetPreflight.canBeginScope(), "reset allows scope begin");
    expectTrue(resetPreflight.canBeginAsyncFlow(), "reset allows async flow begin");
    expectTrue(!resetPreflight.canEndAsyncFlow(), "reset blocks async flow end with no open flows");
    expectTrue(resetPreflight.isNestingHealthy(), "reset nesting state is healthy");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("preflight_nest_outer");
        const fuse::profiler::NestingAsyncFlowPreflight activePreflight =
        expectTrue(!activePreflight.scopeNestingBalanced,
                   "active scope reports unbalanced scope nesting");
        expectTrue(activePreflight.activeScopeNestingDepth == 1u,
                   "preflight reports active scope depth");
        expectTrue(activePreflight.canBeginScope(), "active scope still allows more scopes");
        expectTrue(activePreflight.canBeginAsyncFlow(), "active scope allows async flow begin");

        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_nest_flow", flowId);
        const fuse::profiler::NestingAsyncFlowPreflight openFlowPreflight =
        expectTrue(!openFlowPreflight.flowNestingBalanced,
                   "open flow reports unbalanced flow nesting");
        expectTrue(openFlowPreflight.hasOpenAsyncFlows, "open flow reports hasOpenAsyncFlows");
        expectTrue(openFlowPreflight.openAsyncFlowCount == 1u,
                   "preflight open flow count tracks begin");
        expectTrue(openFlowPreflight.canEndAsyncFlow(), "open flow allows async flow end");
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_nest_flow", flowId);
    }

    const fuse::profiler::NestingAsyncFlowPreflight closedPreflight =
    expectTrue(closedPreflight.scopeNestingBalanced, "scope end restores balanced nesting");
    expectTrue(closedPreflight.flowNestingBalanced, "flow end restores balanced flow nesting");
    expectTrue(closedPreflight.isNestingHealthy(), "balanced trace reports healthy nesting");

void testPreflightNestingAndAsyncFlowCrossThreadHandoff() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_handoff_flow", flowId);

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_handoff_flow", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    const fuse::profiler::NestingAsyncFlowPreflight preflight =
    expectTrue(preflight.flowDepthDetached, "preflight marks detached flow depth after handoff");
    expectTrue(preflight.crossThreadFlowHandoffPending,
               "preflight marks cross-thread handoff pending");
    expectTrue(!preflight.isNestingHealthy(), "handoff leaves nesting unhealthy on begin thread");
    expectTrue(!preflight.canBeginAsyncFlow(),
               "handoff blocks new async flow begin on detached thread");
    expectTrue(!preflight.canEndAsyncFlow(),
               "handoff blocks async flow end when global open count is zero");

    fuse::profiler::reset();
    const fuse::profiler::NestingAsyncFlowPreflight clearedPreflight =
    expectTrue(clearedPreflight.isNestingHealthy(), "reset clears unhealthy nesting state");
    expectTrue(clearedPreflight.canBeginAsyncFlow(), "reset re-enables async flow begin");

void testChromeTraceExportPreflightWithNameAndFlowLookup() {

        FUSE_PROFILE_SCOPE("preflight_lookup_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_lookup_flow", flowId);
        FUSE_PROFILE_COUNTER("preflight_lookup_counter", 6);
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_lookup_flow", flowId);

    const fuse::profiler::ChromeTraceExportPreflight preflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.canExportSafely(), "balanced trace with lookup data can export safely");
    expectTrue(preflight.exportableEventCount == preflight.eventCount,
               "preflight exportable count matches event count");
    expectTrue(fuse::profiler::countEventsByName("preflight_lookup_scope") == 2u,
               "name lookup reconciles with preflight event count");
    expectTrue(fuse::profiler::countEventsByFlowId(flowId) == 2u,
               "flow lookup reconciles with paired flow events");

    fuse::profiler::ProfileEvent scopeBegin{};
    fuse::profiler::ProfileEvent flowFinish{};
    expectTrue(fuse::profiler::tryFirstEventByName("preflight_lookup_scope", scopeBegin),
               "tryFirstEventByName succeeds under export preflight");
    expectTrue(fuse::profiler::tryLastFlowEvent(flowId, flowFinish),
               "tryLastFlowEvent succeeds under export preflight");
    expectTrue(scopeBegin.phase == fuse::profiler::EventPhase::Begin,
               "export preflight path preserves scope begin phase");
    expectTrue(flowFinish.phase == fuse::profiler::EventPhase::FlowFinish,
               "export preflight path preserves flow finish phase");
        fuse::profiler::preflightNestingAsyncFlow();
    expectTrue(resetPreflight.isBalanced(), "nesting preflight balanced on reset");
    expectTrue(resetPreflight.activeScopeNestingDepth == 0u, "nesting preflight scope depth zero on reset");
    expectTrue(resetPreflight.activeFlowNestingDepth == 0u, "nesting preflight flow depth zero on reset");

        FUSE_PROFILE_SCOPE("nesting_preflight_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("nesting_preflight_flow", flowId);
        expectTrue(!activePreflight.isBalanced(), "nesting preflight unbalanced with open scope and flow");
        expectTrue(activePreflight.hasUnbalancedNesting(), "nesting preflight marks unbalanced nesting");
        expectTrue(activePreflight.activeScopeNestingDepth == 1u, "nesting preflight reports scope depth");
        expectTrue(activePreflight.activeFlowNestingDepth == 1u, "nesting preflight reports flow depth");
        expectTrue(activePreflight.openAsyncFlowCount == 1u, "nesting preflight reports open flow count");
        expectTrue(activePreflight.hasOpenAsyncFlows, "nesting preflight marks open async flows");
        FUSE_PROFILE_ASYNC_FLOW_END("nesting_preflight_flow", flowId);

    expectTrue(closedPreflight.isBalanced(), "nesting preflight balanced after teardown");
    expectTrue(!closedPreflight.hasOpenAsyncFlows, "nesting preflight clears open flows after end");

void testOrphanAsyncFlowEndCountGuard() {

    expectTrue(fuse::profiler::orphanAsyncFlowEndCount() == 0u, "orphan count zero on reset");
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_a", 1u);
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_b", 2u);
    expectTrue(fuse::profiler::orphanAsyncFlowEndCount() == 2u, "orphan count tracks ignored finishes");
    expectTrue(fuse::profiler::hasOrphanAsyncFlowEnds(), "hasOrphanAsyncFlowEnds true after orphans");

    const fuse::profiler::ChromeTraceExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.hasOrphanAsyncFlowEnds, "export preflight marks orphan flow ends");
    expectTrue(preflight.orphanAsyncFlowEndCount == 2u, "export preflight orphan count matches");

    expectTrue(fuse::profiler::orphanAsyncFlowEndCount() == 0u, "reset clears orphan count");

void testChromeTraceExportPreflightCanExportWithEvents() {

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight =
    expectTrue(emptyPreflight.canExport(), "empty buffer can still export shell json");
    expectTrue(!emptyPreflight.canExportWithEvents(),
               "canExportWithEvents false when buffer has no exportable events");
    expectTrue(emptyPreflight.hasOnlyExportableEvents == false,
               "hasOnlyExportableEvents false on empty buffer");

        FUSE_PROFILE_SCOPE("preflight_with_events");

    const fuse::profiler::ChromeTraceExportPreflight filledPreflight =
    expectTrue(filledPreflight.canExportWithEvents(),
               "canExportWithEvents true after recording scope begin/end");
    expectTrue(filledPreflight.exportableEventCount == 2u,
               "preflight exportable count matches scope pair");
    expectTrue(filledPreflight.hasOnlyExportableEvents,
               "hasOnlyExportableEvents true when all events exportable");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ChromeTraceExportPreflight disabledPreflight =
    expectTrue(!disabledPreflight.canExportWithEvents(),
               "canExportWithEvents false when profiler disabled");

void testBlankNameLookupDoesNotMatchValidEvents() {

    fuse::profiler::beginAsyncFlow("  ", 1u);
    fuse::profiler::sampleCounter("\t", 1);
        fuse::profiler::ProfileScope blankScope(" ");

    expectTrue(fuse::profiler::findFirstEventIndexByName("valid") == fuse::profiler::kInvalidEventIndex,
               "name lookup stays invalid before valid event");
    expectTrue(fuse::profiler::countEventsByName(" ") == 2u,
               "blank-name lookup finds whitespace scope pair");
    expectTrue(fuse::profiler::countEventsByName("\t") == 1u,
               "blank-name lookup finds whitespace counter");

    FUSE_PROFILE_COUNTER("valid", 5);
    expectTrue(fuse::profiler::findFirstEventIndexByName("valid") == 4u,
               "name lookup finds valid counter after blank-name events");

    fuse::profiler::ProfileEvent counter{};
    expectTrue(fuse::profiler::tryFindFirstEventByName("valid", counter),
               "tryFindFirstEventByName succeeds for valid counter after blank names");
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
    testFlowNestingDepthIntrospection();
    testOpenAsyncFlowCountTracking();
    testLastEventIndexAndLastEvent();
    testBufferEmptyAndFullGuards();
    testOrphanAsyncFlowEndIsIgnored();
    testNullNameFlowAndCounterGuards();
    testNullScopeNameGuard();
    testDisabledProfilerDoesNotMutateScopeNestingDepth();
    testDisabledAsyncFlowBeginSkipsDepthAndOpenCount();
    testResetClearsOpenAsyncFlowCount();
    testDisabledCounterPreservesFlowNestingDepth();
    testScopeNestingDepthIntrospection();
    testOpenAsyncFlowCountIntrospection();
    testNullNameProfileScopeGuard();
    testResetClearsNestingAndFlowGuardState();
    testDisabledScopeDoesNotMutateNestingDepth();
    testMultipleOrphanAsyncFlowEndsAreIgnored();
    testDisabledBeginAsyncFlowDoesNotIncrementOpenCount();
    testEmptyStringNameGuards();
    testNestingBalanceIntrospection();
    testTryEventAtGuard();
    testResetRestoresNestingBalance();
    testIsValidEventNameGuard();
    testRingCapacityAndEmptyProfileEventSentinel();
    testTryLastEventGuard();
    testChromeTraceExportPreflightEmptyBuffer();
    testChromeTraceExportPreflightWithEvents();
    testChromeTraceExportPreflightOpenFlows();
    testChromeTraceExportPreflightDisabledProfiler();
    testCrossThreadFlowPreservesOpenCountGuard();
    testExportableEventCountGuard();
    testIsEventExportableGuard();
    testFirstEventIndexAndTryFirstEventGuard();
    testHasUnbalancedNestingGuard();
    testIsFlowDepthDetachedGuard();
    testMixedEmptyAndValidNameGuards();
    testDisabledEndAsyncFlowPreservesOpenCount();
    testChromeTraceExportPreflightActiveScope();
    testChromeTraceExportPreflightNestingDepths();
    testChromeTraceExportPreflightDetachedFlow();
    testIsProfileEventSentinelGuard();
    testInvalidNameEventCountGuard();
    testTryExportableEventAtGuard();
    testFindEventIndexByPhaseGuard();
    testIsCrossThreadFlowHandoffPendingGuard();
    testChromeTraceExportPreflightSafetyFlags();
    testChromeTraceExportPreflightCrossThreadHandoff();
    testEmptyNameAttemptsDoNotAffectPhaseLookup();
    testFindEventIndexByNameGuard();
    testTryEventByNameGuard();
    testFindEventIndexByFlowIdGuard();
    testTryFlowEventGuard();
    testEventNameMatchesGuard();
    testEmptyNameAttemptsDoNotAffectNameAndFlowLookup();
    testPreflightNestingAndAsyncFlowBalanced();
    testPreflightNestingAndAsyncFlowCrossThreadHandoff();
    testChromeTraceExportPreflightWithNameAndFlowLookup();
    testIsBlankEventNameGuard();
    testTryFindEventByNameGuard();
    testTryFindFlowEventGuard();
    testTryExportableFirstAndLastEventGuard();
    testTryFindEventIndexGuards();
    testPreflightProfileScopeGuard();
    testPreflightAsyncFlowGuards();
    testPreflightNestingAsyncFlowGuard();
    testOrphanAsyncFlowEndCountGuard();
    testChromeTraceExportPreflightCanExportWithEvents();
    testBlankNameLookupDoesNotMatchValidEvents();
    testFatalHandlerHook();
    testVerifyMacro();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_profiler_assert: all tests passed\n");
    return EXIT_SUCCESS;
}
