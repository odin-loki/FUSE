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
    expectTrue(!fuse::profiler::isEventIndexValid(0), "isEventIndexValid false on empty buffer");
    expectTrue(!fuse::profiler::isEventIndexValid(99), "isEventIndexValid false when out of range");

    const fuse::profiler::ProfileEvent& emptyEvent = fuse::profiler::eventAt(0);
    expectTrue(emptyEvent.name == nullptr, "eventAt on empty buffer returns sentinel with null name");
    expectTrue(!fuse::profiler::isValidProfileEvent(emptyEvent),
               "isValidProfileEvent false for empty-buffer sentinel");
    expectTrue(emptyEvent.phase == fuse::profiler::EventPhase::Begin,
               "eventAt sentinel keeps default begin phase");
    expectTrue(&emptyEvent == &fuse::profiler::emptyProfileEvent(),
               "eventAt sentinel matches emptyProfileEvent()");
               "eventAt on empty buffer returns emptyProfileEvent sentinel");

    const fuse::profiler::ProfileEvent& oobEvent = fuse::profiler::eventAt(99);
    expectTrue(oobEvent.name == nullptr, "eventAt out-of-range returns sentinel with null name");
    expectTrue(!fuse::profiler::isValidProfileEvent(oobEvent),
               "isValidProfileEvent false for out-of-range sentinel");
    expectTrue(&oobEvent == &fuse::profiler::emptyProfileEvent(),
               "out-of-range eventAt returns emptyProfileEvent()");
               "eventAt out-of-range returns emptyProfileEvent sentinel");

    {
        FUSE_PROFILE_SCOPE("guard_scope");
    }

    expectTrue(fuse::profiler::hasEvents(), "hasEvents true after recording scope");
    expectTrue(fuse::profiler::isEventIndexValid(0u), "isEventIndexValid true for first event");
    expectTrue(fuse::profiler::isEventIndexValid(1u), "isEventIndexValid true for last event");
    expectTrue(!fuse::profiler::isEventIndexValid(2u), "isEventIndexValid false past event count");
    expectTrue(fuse::profiler::isValidProfileEvent(fuse::profiler::eventAt(0)),
               "isValidProfileEvent true for recorded event");
    expectTrue(fuse::profiler::isEventIndexValid(0), "isEventIndexValid true for first event");
    expectTrue(fuse::profiler::isEventIndexValid(1), "isEventIndexValid true for last event");
    expectTrue(!fuse::profiler::isEventIndexValid(2), "isEventIndexValid false past event count");
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

void testDisabledProfilerEndBalancesActiveFlowDepth() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("mid_disable_flow", flowId);
    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_ASYNC_FLOW_END("mid_disable_flow", flowId);

    expectTrue(fuse::profiler::eventCount() == 1u,
               "disabled flow end keeps the recorded begin without emitting finish");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 1u,
               "disabled flow end still balanced the active flow depth");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("clean_flow", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("clean_flow", flowId);
    expectTrue(fuse::profiler::eventCount() == 3u,
               "re-enabled profiler records a balanced flow pair after mid-disable cleanup");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 1u,
               "flow nesting depth remains clean after re-enable");
}

void testDisabledProfilerSkipsSnapshotCounter() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_snapshot", 512);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_float_snapshot", 1.25);

    expectTrue(fuse::profiler::eventCount() == 0u,
               "disabled profiler skips snapshot_at_frame counter samples");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "disabled snapshot counter export stays empty");
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
void testScopeNestingDepthIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "reset leaves scope nesting depth at zero");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "reset leaves nestingDepth alias at zero");

    {
        FUSE_PROFILE_SCOPE("depth_outer");
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "outer scope increments introspection depth");
            FUSE_PROFILE_SCOPE("depth_inner");
            expectTrue(fuse::profiler::scopeNestingDepth() == 2u,
                       "nested scope increments introspection depth");
        }
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "inner scope end restores introspection depth");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "outer scope end clears introspection depth");
        expectTrue(fuse::profiler::nestingDepth() == 1u, "nestingDepth alias tracks outer scope");
        {
            expectTrue(fuse::profiler::scopeNestingDepth() == 2u, "inner scope increments introspection depth");
            expectTrue(fuse::profiler::nestingDepth() == 2u, "nestingDepth alias tracks inner scope");
        expectTrue(fuse::profiler::nestingDepth() == 1u, "nestingDepth alias restores after inner scope");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "nestingDepth alias clears after outer scope");

void testRingCapacityAndBufferFullGuards() {

    expectTrue(fuse::profiler::ringCapacity() == 4096u, "ring capacity exposes compile-time buffer size");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringCapacity(),
               "event count stays below ring capacity on empty buffer");

        FUSE_PROFILE_SCOPE("capacity_probe");

    expectTrue(!fuse::profiler::isBufferFull(), "small trace does not fill ring buffer");
               "recorded events remain below ring capacity");

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

void testLastEventIndexAndLastEvent() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::lastEventIndex() == fuse::profiler::kInvalidEventIndex,
               "lastEventIndex is invalid on empty buffer");
    expectTrue(!fuse::profiler::isValidProfileEvent(fuse::profiler::lastEvent()),
               "lastEvent returns sentinel on empty buffer");

    {
        FUSE_PROFILE_SCOPE("first_scope");
    expectTrue(fuse::profiler::lastEventIndex() == 1u, "lastEventIndex points at scope end");
    expectTrue(fuse::profiler::isValidProfileEvent(fuse::profiler::lastEvent()),
               "lastEvent is valid after recording");
    expectTrue(fuse::profiler::lastEvent().phase == fuse::profiler::EventPhase::End,
               "lastEvent returns most recent end phase");
    expectTrue(std::string(fuse::profiler::lastEvent().name) == "first_scope",
               "lastEvent preserves most recent scope name");

void testBufferEmptyAndFullGuards() {

    expectTrue(fuse::profiler::isBufferEmpty(), "reset leaves buffer empty");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(!fuse::profiler::hasEvents(), "isBufferEmpty mirrors hasEvents on reset");

        FUSE_PROFILE_SCOPE("single_scope");

    expectTrue(!fuse::profiler::isBufferEmpty(), "recorded events clear isBufferEmpty");
    expectTrue(fuse::profiler::hasEvents(), "hasEvents true after recording");
    expectTrue(!fuse::profiler::isBufferFull(), "two events do not saturate ring buffer");

void testDisabledProfilerDoesNotMutateScopeNestingDepth() {

    fuse::profiler::setEnabled(false);
        FUSE_PROFILE_SCOPE("ignored_outer");
            FUSE_PROFILE_SCOPE("ignored_inner");

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested scope events");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u,
               "disabled profiler does not mutate scope nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled profiler does not bump max scope nesting depth");

    fuse::profiler::setEnabled(true);
        FUSE_PROFILE_SCOPE("enabled_scope");
    expectTrue(fuse::profiler::maxNestingDepth() == 1u,
               "scope nesting depth resumes cleanly after re-enable");

void testDisabledAsyncFlowBeginSkipsDepthAndOpenCount() {

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_begin", flowId);

    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "disabled flow begin does not mutate flow nesting depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "disabled flow begin does not increment open async flow count");
    expectTrue(fuse::profiler::eventCount() == 0u, "disabled flow begin records no event");

void testNullScopeNameGuard() {
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false when no flows are open");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("tracked_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open async flow count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true while flow is open");

    FUSE_PROFILE_ASYNC_FLOW_END("tracked_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "flow end clears open async flow count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after flow closes");

void testNullNameScopeGuard() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope nullScope(nullptr);
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "null scope name records nothing");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "null scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "null scope name does not mutate scope depth");
    expectTrue(!fuse::profiler::hasEvents(), "null scope name leaves buffer empty");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null scope name leaves export empty");
}

void testResetClearsOpenAsyncFlowCount() {
void testDisabledAsyncFlowBeginDoesNotOpenFlow() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_begin", flowId);

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "disabled flow begin does not increment open async flow count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "disabled flow begin does not mark flows open");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "disabled flow begin does not mutate flow depth");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("enabled_begin", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "enabled flow begin increments open async flow count after re-enable");
    FUSE_PROFILE_ASYNC_FLOW_END("enabled_begin", flowId);
}

void testResetClearsOpenAsyncFlowState() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open flow count tracks begin");

    fuse::profiler::reset();
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset clears open async flow count");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset clears event buffer");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset clears flow nesting depth");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "flow open before reset");

    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset clears hasOpenAsyncFlows");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "reset clears scope nesting depth");
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
            FUSE_PROFILE_SCOPE("depth_inner");
            expectTrue(fuse::profiler::nestingDepth() == 2u, "inner scope increments introspection depth");
        }
        expectTrue(fuse::profiler::nestingDepth() == 1u, "inner scope end restores introspection depth");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "outer scope end clears introspection depth");

void testOpenAsyncFlowCountIntrospection() {
        expectTrue(fuse::profiler::nestingDepth() == 1u, "inner scope exit restores introspection depth");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "outer scope exit clears introspection depth");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "reset leaves scope nesting depth at zero");

        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "outer scope increments introspection depth");
            expectTrue(fuse::profiler::scopeNestingDepth() == 2u, "inner scope increments introspection depth");
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "inner scope end restores introspection depth");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "outer scope end clears introspection depth");

void testDisabledScopeDoesNotMutateScopeNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
        FUSE_PROFILE_SCOPE("ignored_scope");
        expectTrue(fuse::profiler::scopeNestingDepth() == 0u,
                   "disabled scope does not mutate scope nesting depth");
               "disabled scope destructor does not underflow scope depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled scope does not bump max nesting depth");

    fuse::profiler::setEnabled(true);
        FUSE_PROFILE_SCOPE("enabled_scope");
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "scope nesting resumes cleanly after re-enable");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "enabled scope restores depth after re-enable");

void testOpenAsyncFlowCountGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset leaves hasOpenAsyncFlows false");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset leaves open async flow count at zero");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_outer", outerFlowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "flow begin sets hasOpenAsyncFlows");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 2u, "nested flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "inner flow end decrements open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_outer", outerFlowId);
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "outer flow end clears hasOpenAsyncFlows");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "outer flow end clears open count");
}

void testNullNameProfileScopeGuard() {
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open async flow count");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow end decrements open async flow count");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "final flow end clears open async flow count");

void testNullNameScopeGuard() {
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

void testResetClearsNestingAndFlowGuardState() {
void testDisabledAsyncFlowEndPreservesOpenCount() {

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin establishes open count");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_ASYNC_FLOW_END("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "disabled flow finish preserves open async flow count");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled flow finish preserves thread-local flow depth");

    fuse::profiler::setEnabled(true);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "re-enabled flow finish clears open async flow count");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "re-enabled flow finish restores thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow events recorded after disable guard");

void testDoubleOrphanAsyncFlowEndIsIgnored() {

    FUSE_PROFILE_ASYNC_FLOW_END("first_orphan", 11u);
    FUSE_PROFILE_ASYNC_FLOW_END("second_orphan", 12u);

    expectTrue(fuse::profiler::eventCount() == 0u, "double orphan flow finish records nothing");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "double orphan flow finish keeps open count at zero");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "double orphan flow finish does not underflow depth");

void testCrossThreadFlowFinishSkipsWorkerFlowDepthPop() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "begin thread records flow depth");

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
                   "worker thread starts with zero flow depth");
        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_flow", flowId);
                   "cross-thread flow finish does not underflow worker flow depth");
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

               "cross-thread flow finish clears global open count");
               "begin thread flow depth remains after cross-thread finish stub");

    fuse::profiler::reset();
               "reset clears begin-thread flow depth after cross-thread handoff");

void testResetClearsNestingAndOpenFlowState() {
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
        FUSE_PROFILE_SCOPE("reset_scope");
        FUSE_PROFILE_COUNTER("reset_counter", 1);

    expectTrue(fuse::profiler::hasEvents(), "guard reset pre-check records events");

    expectTrue(!fuse::profiler::hasEvents(), "reset clears hasEvents");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "reset clears scope nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset clears flow nesting depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset clears open async flow count");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u, "reset clears max nesting depth");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 0u, "reset clears max flow nesting depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "reset leaves chrome export empty");
}

void testDisabledScopeDoesNotMutateNestingDepth() {
               "reset clears export buffer");

void testDoubleOrphanAsyncFlowEndIsIgnored() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_ASYNC_FLOW_END("orphan_one", 11u);
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_two", 12u);

    expectTrue(fuse::profiler::eventCount() == 0u, "double orphan flow finish records no events");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "double orphan finish leaves open count at zero");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "double orphan finish does not underflow flow depth");

void testDisabledScopePreservesNestingDepth() {

    fuse::profiler::setEnabled(false);
    {
        FUSE_PROFILE_SCOPE("ignored_outer");
            FUSE_PROFILE_SCOPE("ignored_inner");

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested scope events");
    expectTrue(fuse::profiler::nestingDepth() == 0u,
               "disabled profiler does not mutate scope nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled profiler does not bump max nesting depth");

    fuse::profiler::setEnabled(true);
        FUSE_PROFILE_SCOPE("enabled_scope");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "scope nesting depth resumes cleanly after re-enable");
    expectTrue(fuse::profiler::maxNestingDepth() == 1u,
               "scope nesting depth resumes recording after re-enable");

void testMultipleOrphanAsyncFlowEndsAreIgnored() {

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

void testDisabledBeginAsyncFlowDoesNotIncrementOpenCount() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_begin", flowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_nested", flowId + 1u);

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled flow begin records nothing");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "disabled flow begin does not increment open count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "disabled flow begin leaves hasOpenAsyncFlows false");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "disabled flow begin leaves hasOpenAsyncFlows false");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "disabled flow begin does not mutate flow nesting depth");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("enabled_flow", flowId);
               "open count resumes cleanly after re-enable");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "enabled flow begin sets hasOpenAsyncFlows");
    FUSE_PROFILE_ASYNC_FLOW_END("enabled_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "enabled flow pair clears open count");

void testEmptyStringNameGuards() {

        fuse::profiler::ProfileScope emptyScope("");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "enabled flow pair clears hasOpenAsyncFlows");
}

    resetState();
    fuse::platform::registerMainThread();

    {
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
               "empty-string guard leaves export empty");

void testNestingBalanceIntrospection() {
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
}

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
    expectTrue(fuse::profiler::isScopeNestingBalanced(), "scope end restores balanced nesting");

void testTryEventAtGuard() {
    }

    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryEventAt clears output on empty buffer");
    expectTrue(!fuse::profiler::isValidProfileEvent(outEvent),
               "tryEventAt output is invalid on empty buffer");

        FUSE_PROFILE_SCOPE("try_scope");
    {
    }

    expectTrue(fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt true for first event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "try_scope",
               "tryEventAt copies event name");

    expectTrue(fuse::profiler::tryEventAt(1u, outEvent), "tryEventAt true for last event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryEventAt copies end phase");

    expectTrue(!fuse::profiler::tryEventAt(2u, outEvent), "tryEventAt false past event count");
    expectTrue(outEvent.name == nullptr, "tryEventAt clears output when out of range");

void testResetRestoresNestingBalance() {

        FUSE_PROFILE_SCOPE("reset_balance_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_balance_flow", flowId);
}

    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {

    expectTrue(fuse::profiler::isScopeNestingBalanced(),
               "ended scope restores nesting balance even with open flow");
    expectTrue(!fuse::profiler::isFlowNestingBalanced(), "unmatched flow leaves flow nesting unbalanced");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "unmatched flow leaves open async flows");

    fuse::profiler::reset();

    expectTrue(fuse::profiler::isScopeNestingBalanced(), "reset restores scope nesting balance");
    expectTrue(fuse::profiler::isFlowNestingBalanced(), "reset restores flow nesting balance");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset clears open async flows");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset clears buffer after unmatched nesting");

void testIsValidEventNameGuard() {

    expectTrue(!fuse::profiler::isValidEventName(nullptr), "null name is invalid");
    expectTrue(!fuse::profiler::isValidEventName(""), "empty string name is invalid");
    expectTrue(fuse::profiler::isValidEventName("scope"), "non-empty name is valid");
    expectTrue(!fuse::profiler::isValidProfileEvent(fuse::profiler::emptyProfileEvent()),
               "emptyProfileEvent fails isValidProfileEvent");

void testRingCapacityAndEmptyProfileEventSentinel() {

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

void testTryLastEventGuard() {

    expectTrue(!fuse::profiler::tryLastEvent(outEvent), "tryLastEvent false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryLastEvent clears output on empty buffer");

        FUSE_PROFILE_SCOPE("try_last_scope");

    expectTrue(fuse::profiler::tryLastEvent(outEvent), "tryLastEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastEvent copies last end phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "try_last_scope",
               "tryLastEvent copies last scope name");

void testChromeTraceExportPreflightEmptyBuffer() {

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

void testChromeTraceExportPreflightWithEvents() {

    fuse::profiler::beginFrame();
        FUSE_PROFILE_SCOPE("preflight_scope");
        FUSE_PROFILE_COUNTER("preflight_counter", 9);

    expectTrue(preflight.canExport(), "preflight allows export with recorded events");
    expectTrue(!preflight.bufferEmpty, "preflight marks non-empty buffer");
    expectTrue(preflight.eventCount == 3u, "preflight counts scope begin/end and counter");
    expectTrue(preflight.exportableEventCount == 3u, "preflight counts exportable events");
    expectTrue(preflight.hasExportableEvents(), "preflight hasExportableEvents true with trace data");
    expectTrue(preflight.frameIndex == 1u, "preflight frame index reflects beginFrame");
    expectTrue(preflight.scopeNestingUnbalanced == false, "preflight scope nesting balanced after scope end");
    expectTrue(preflight.flowNestingUnbalanced == false, "preflight flow nesting balanced with no open flows");

void testChromeTraceExportPreflightOpenFlows() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_flow", flowId);

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

void testChromeTraceExportPreflightDisabledProfiler() {

    expectTrue(!preflight.canExport(), "preflight blocks export when profiler disabled");
    expectTrue(preflight.profilerDisabled, "preflight marks profiler disabled");
    expectTrue(preflight.bufferEmpty, "preflight buffer empty when disabled");
    expectTrue(!preflight.hasExportableEvents(), "preflight hasExportableEvents false when disabled");

void testCrossThreadFlowPreservesOpenCountGuard() {
}

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

void testExportableEventCountGuard() {

    expectTrue(fuse::profiler::exportableEventCount() == 0u,
               "exportableEventCount is zero on empty buffer");

        FUSE_PROFILE_SCOPE("exportable_scope");
        FUSE_PROFILE_COUNTER("exportable_counter", 4);

    expectTrue(fuse::profiler::exportableEventCount() == 3u,
               "exportableEventCount counts scope begin/end and counter");
    expectTrue(fuse::profiler::exportableEventCount() == fuse::profiler::eventCount(),
               "exportableEventCount matches eventCount for valid names");

    fuse::profiler::beginAsyncFlow(nullptr, 1u);
               "null-name flow attempt does not affect exportableEventCount");

void testIsEventExportableGuard() {

    expectTrue(!fuse::profiler::isEventExportable(0u), "isEventExportable false on empty buffer");
    expectTrue(!fuse::profiler::isEventExportable(99u), "isEventExportable false when out of range");

        FUSE_PROFILE_SCOPE("exportable_event");

    expectTrue(fuse::profiler::isEventExportable(0u), "isEventExportable true for begin event");
    expectTrue(fuse::profiler::isEventExportable(1u), "isEventExportable true for end event");
    expectTrue(!fuse::profiler::isEventExportable(2u), "isEventExportable false past event count");

void testFirstEventIndexAndTryFirstEventGuard() {

    expectTrue(fuse::profiler::firstEventIndex() == fuse::profiler::kInvalidEventIndex,
               "firstEventIndex invalid on empty buffer");

    expectTrue(!fuse::profiler::tryFirstEvent(outEvent), "tryFirstEvent false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryFirstEvent clears output on empty buffer");

        FUSE_PROFILE_SCOPE("first_event_scope");

    expectTrue(fuse::profiler::firstEventIndex() == 0u, "firstEventIndex is zero after recording");
    expectTrue(fuse::profiler::tryFirstEvent(outEvent), "tryFirstEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryFirstEvent copies begin phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "first_event_scope",
               "tryFirstEvent copies first scope name");

void testHasUnbalancedNestingGuard() {

    expectTrue(!fuse::profiler::hasUnbalancedNesting(), "reset leaves nesting balanced");

        FUSE_PROFILE_SCOPE("balance_probe");
        expectTrue(fuse::profiler::hasUnbalancedNesting(), "active scope reports unbalanced nesting");
        expectTrue(fuse::profiler::hasUnbalancedNesting(), "open flow inside scope stays unbalanced");
        expectTrue(fuse::profiler::hasUnbalancedNesting(), "active scope still unbalanced after flow end");
    expectTrue(!fuse::profiler::hasUnbalancedNesting(), "ended scope restores balanced nesting");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("unmatched_flow", flowId);
    expectTrue(fuse::profiler::hasUnbalancedNesting(), "unmatched flow reports unbalanced nesting");

void testIsFlowDepthDetachedGuard() {

    expectTrue(!fuse::profiler::isFlowDepthDetached(), "reset leaves flow depth attached");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("attached_flow", flowId);
    expectTrue(!fuse::profiler::isFlowDepthDetached(), "same-thread flow keeps depth attached");
    FUSE_PROFILE_ASYNC_FLOW_END("attached_flow", flowId);
    expectTrue(!fuse::profiler::isFlowDepthDetached(), "paired flow end clears detached state");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_detach", flowId);
    expectTrue(!fuse::profiler::isFlowDepthDetached(), "begin on main keeps depth attached");

        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_detach", flowId);

    expectTrue(fuse::profiler::isFlowDepthDetached(),
               "cross-thread finish leaves begin-thread flow depth detached");
               "begin-thread flow depth remains until same-thread cleanup or reset");
               "cross-thread finish clears global open async flow count");

void testMixedEmptyAndValidNameGuards() {

        fuse::profiler::ProfileScope nullScope(nullptr);
    fuse::profiler::sampleCounter("", 1);

    expectTrue(fuse::profiler::eventCount() == 0u, "empty/null names record nothing before valid event");

        FUSE_PROFILE_SCOPE("valid_after_empty");
        FUSE_PROFILE_COUNTER("valid_counter", 2);

    expectTrue(fuse::profiler::eventCount() == 3u, "valid events record after empty-name attempts");
               "exportable count matches after mixed empty/valid attempts");
    expectTrue(fuse::profiler::isScopeNestingBalanced(), "valid scope nesting balanced after empty attempts");
    expectTrue(fuse::profiler::isFlowNestingBalanced(), "valid flow nesting balanced after empty attempts");

void testDisabledEndAsyncFlowPreservesOpenCount() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("disable_guard_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open count");

    FUSE_PROFILE_ASYNC_FLOW_END("disable_guard_flow", flowId);

               "disabled flow end does not decrement open count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "disabled flow end leaves open async flows");
               "disabled flow end does not mutate thread-local flow depth");

               "enabled flow end clears open count after disable guard");

void testChromeTraceExportPreflightActiveScope() {

        FUSE_PROFILE_SCOPE("preflight_active_scope");
        expectTrue(preflight.canExport(), "preflight allows export inside active scope");
        expectTrue(preflight.scopeNestingUnbalanced, "preflight marks active scope as unbalanced");
        expectTrue(preflight.hasUnbalancedNesting(), "preflight hasUnbalancedNesting inside active scope");
        expectTrue(preflight.activeScopeNestingDepth == 1u, "preflight reports active scope depth");
        expectTrue(preflight.maxScopeNestingDepth == 1u, "preflight reports max scope depth");
        expectTrue(preflight.exportableEventCount == 1u, "preflight counts begin event inside active scope");
        expectTrue(preflight.eventCount == 1u, "preflight event count includes active begin");

    expectTrue(!closedPreflight.scopeNestingUnbalanced, "preflight scope balanced after scope end");
    expectTrue(!closedPreflight.hasUnbalancedNesting(), "preflight nesting balanced after scope end");
    expectTrue(closedPreflight.exportableEventCount == 2u, "preflight counts paired scope events");

void testChromeTraceExportPreflightNestingDepths() {

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
        FUSE_PROFILE_SCOPE("preflight_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_outer_flow", outerFlowId);
            FUSE_PROFILE_SCOPE("preflight_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_inner_flow", innerFlowId);
            expectTrue(preflight.activeScopeNestingDepth == 2u, "preflight reports inner scope depth");
            expectTrue(preflight.activeFlowNestingDepth == 2u, "preflight reports nested flow depth");
            expectTrue(preflight.maxScopeNestingDepth == 2u, "preflight max scope depth tracks inner scope");
            expectTrue(preflight.maxFlowNestingDepth == 2u, "preflight max flow depth tracks inner flow");
            expectTrue(preflight.openAsyncFlowCount == 2u, "preflight open flow count tracks nested begins");
            expectTrue(preflight.hasUnbalancedNesting(), "preflight nesting unbalanced with open scopes/flows");
            FUSE_PROFILE_ASYNC_FLOW_END("preflight_inner_flow", innerFlowId);
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_outer_flow", outerFlowId);

    expectTrue(closedPreflight.activeScopeNestingDepth == 0u, "preflight active scope depth clears after end");
    expectTrue(closedPreflight.activeFlowNestingDepth == 0u, "preflight active flow depth clears after end");
    expectTrue(!closedPreflight.hasUnbalancedNesting(), "preflight nesting balanced after nested teardown");

void testChromeTraceExportPreflightDetachedFlow() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_detach", flowId);

        FUSE_PROFILE_ASYNC_FLOW_END("preflight_detach", flowId);

    expectTrue(preflight.flowDepthDetached, "preflight marks detached flow depth after cross-thread end");
    expectTrue(!preflight.hasOpenAsyncFlows, "preflight has no open flows after cross-thread end");
    expectTrue(preflight.flowNestingUnbalanced, "preflight flow nesting unbalanced on begin thread");
    expectTrue(preflight.activeFlowNestingDepth == 1u, "preflight active flow depth remains on begin thread");
    expectTrue(preflight.openAsyncFlowCount == 0u, "preflight open flow count cleared by worker end");

void testIsProfileEventSentinelGuard() {

    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::emptyProfileEvent()),
               "emptyProfileEvent is a sentinel");
    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::eventAt(0)),
               "eventAt on empty buffer returns sentinel shape");
    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::lastEvent()),
               "lastEvent on empty buffer returns sentinel shape");

    fuse::profiler::ProfileEvent cleared{};
    expectTrue(fuse::profiler::isProfileEventSentinel(cleared),
               "default-constructed event matches sentinel shape");

        FUSE_PROFILE_SCOPE("sentinel_scope");

    expectTrue(!fuse::profiler::isProfileEventSentinel(fuse::profiler::eventAt(0)),
               "recorded begin event is not a sentinel");
    expectTrue(fuse::profiler::isProfileEventSentinel(fuse::profiler::eventAt(99)),
               "out-of-range eventAt still returns sentinel shape");

void testInvalidNameEventCountGuard() {

    expectTrue(fuse::profiler::invalidNameEventCount() == 0u,
               "invalidNameEventCount is zero on empty buffer");
    expectTrue(!fuse::profiler::hasInvalidNameEvents(), "hasInvalidNameEvents false on empty buffer");

        FUSE_PROFILE_SCOPE("valid_scope");
        FUSE_PROFILE_COUNTER("valid_counter", 3);

               "valid events keep invalidNameEventCount at zero");
    expectTrue(!fuse::profiler::hasInvalidNameEvents(), "hasInvalidNameEvents false for valid trace");
    expectTrue(fuse::profiler::invalidNameEventCount() + fuse::profiler::exportableEventCount()
                   == fuse::profiler::eventCount(),
               "invalid + exportable counts reconcile with eventCount");

void testTryExportableEventAtGuard() {

    expectTrue(!fuse::profiler::tryExportableEventAt(0u, outEvent),
               "tryExportableEventAt false on empty buffer");
    expectTrue(fuse::profiler::isProfileEventSentinel(outEvent),
               "tryExportableEventAt clears output on empty buffer");


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
               "tryExportableEventAt clears output when out of range");

void testFindEventIndexByPhaseGuard() {

    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::Begin)
                   == fuse::profiler::kInvalidEventIndex,
               "findFirstEventIndexByPhase invalid on empty buffer");
    expectTrue(fuse::profiler::findLastEventIndexByPhase(fuse::profiler::EventPhase::End)
               "findLastEventIndexByPhase invalid on empty buffer");
    expectTrue(fuse::profiler::countEventsByPhase(fuse::profiler::EventPhase::Counter) == 0u,
               "countEventsByPhase is zero on empty buffer");

        FUSE_PROFILE_SCOPE("phase_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("phase_flow", flowId);
        FUSE_PROFILE_COUNTER("phase_counter", 5);
        FUSE_PROFILE_ASYNC_FLOW_END("phase_flow", flowId);

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

void testIsCrossThreadFlowHandoffPendingGuard() {

    expectTrue(!fuse::profiler::isCrossThreadFlowHandoffPending(),
               "reset leaves cross-thread handoff pending false");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("handoff_flow", flowId);
               "same-thread open flow is not a pending handoff");

        FUSE_PROFILE_ASYNC_FLOW_END("handoff_flow", flowId);

               "cross-thread finish leaves flow depth detached");
    expectTrue(fuse::profiler::isCrossThreadFlowHandoffPending(),
               "cross-thread finish reports pending handoff on begin thread");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "cross-thread finish clears global open flow count");

               "reset clears cross-thread handoff pending state");

void testChromeTraceExportPreflightSafetyFlags() {

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.canExportSafely(), "empty balanced buffer can export safely");
    expectTrue(!emptyPreflight.hasInvalidNameEvents, "empty preflight has no invalid-name events");
    expectTrue(emptyPreflight.invalidNameEventCount == 0u, "empty preflight invalid-name count is zero");
    expectTrue(!emptyPreflight.ringBufferFull, "empty preflight ring buffer is not full");
    expectTrue(!emptyPreflight.crossThreadFlowHandoffPending,
               "empty preflight has no pending cross-thread handoff");

        FUSE_PROFILE_SCOPE("preflight_safe_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_safe_flow", flowId);
        const fuse::profiler::ChromeTraceExportPreflight activePreflight =
        expectTrue(activePreflight.canExport(), "preflight allows export with open scope and flow");
        expectTrue(!activePreflight.canExportSafely(),
                   "preflight blocks safe export with unbalanced nesting");
        expectTrue(activePreflight.hasUnbalancedNesting(),
                   "preflight marks active scope/flow as unbalanced");
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_safe_flow", flowId);

    const fuse::profiler::ChromeTraceExportPreflight closedPreflight =
    expectTrue(closedPreflight.canExportSafely(), "balanced trace can export safely");
    expectTrue(closedPreflight.exportableEventCount == closedPreflight.eventCount,
               "safe preflight exportable count matches event count");

void testChromeTraceExportPreflightCrossThreadHandoff() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_handoff", flowId);

        FUSE_PROFILE_ASYNC_FLOW_END("preflight_handoff", flowId);

    expectTrue(preflight.crossThreadFlowHandoffPending,
               "preflight marks pending cross-thread handoff");
    expectTrue(preflight.flowDepthDetached, "preflight flow depth detached after cross-thread end");
    expectTrue(!preflight.canExportSafely(), "preflight blocks safe export during handoff cleanup");
    expectTrue(preflight.canExport(), "preflight still allows raw export during handoff cleanup");

void testEmptyNameAttemptsDoNotAffectPhaseLookup() {


    expectTrue(fuse::profiler::eventCount() == 0u, "empty-name attempts record nothing");
    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::Counter)
               "phase lookup stays invalid after empty-name attempts");

    FUSE_PROFILE_COUNTER("valid_after_empty", 4);
    expectTrue(fuse::profiler::findFirstEventIndexByPhase(fuse::profiler::EventPhase::Counter) == 0u,
               "phase lookup finds valid counter after empty-name attempts");

    fuse::profiler::ProfileEvent counterEvent{};
    expectTrue(fuse::profiler::tryExportableEventAt(0u, counterEvent),
               "tryExportableEventAt succeeds for valid counter after empty-name attempts");
    expectTrue(counterEvent.phase == fuse::profiler::EventPhase::Counter,
               "exportable lookup copies counter phase after empty-name attempts");

void testIsBlankEventNameGuard() {

    expectTrue(fuse::profiler::isBlankEventName(nullptr), "null name is blank");
    expectTrue(fuse::profiler::isBlankEventName(""), "empty string is blank");
    expectTrue(fuse::profiler::isBlankEventName(" \t\r\n"), "whitespace-only name is blank");
    expectTrue(!fuse::profiler::isBlankEventName("scope"), "non-blank name is not blank");
    expectTrue(fuse::profiler::isValidEventName(" \t"), "whitespace-only name still passes isValidEventName");
    expectTrue(fuse::profiler::isBlankEventName(" \t"), "whitespace-only name fails isBlankEventName");

        fuse::profiler::ProfileScope whitespaceScope(" \t");
    expectTrue(fuse::profiler::eventCount() == 2u,
               "whitespace-only scope name still records on valid path");
    expectTrue(fuse::profiler::countEventsByName(" \t") == 2u,
               "name lookup finds whitespace-only scope events");

void testFindEventIndexByNameGuard() {

    expectTrue(fuse::profiler::findFirstEventIndexByName(nullptr) == fuse::profiler::kInvalidEventIndex,
               "findFirstEventIndexByName rejects null name");
    expectTrue(fuse::profiler::findLastEventIndexByName("") == fuse::profiler::kInvalidEventIndex,
               "findLastEventIndexByName rejects empty name");
    expectTrue(fuse::profiler::countEventsByName(nullptr) == 0u,
               "countEventsByName returns zero for null name");
    expectTrue(!fuse::profiler::hasEventsWithName(""), "hasEventsWithName false for empty name");

        FUSE_PROFILE_SCOPE("name_lookup_outer");
        FUSE_PROFILE_COUNTER("name_lookup_counter", 1);
            FUSE_PROFILE_SCOPE("name_lookup_inner");

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

    expectTrue(!fuse::profiler::tryFirstEventByName(nullptr, outEvent),
               "tryFirstEventByName false for null name");
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

void testFindEventIndexByFlowIdGuard() {

    expectTrue(!fuse::profiler::isValidFlowId(0u), "flow id zero is invalid");
    expectTrue(fuse::profiler::isValidFlowId(1u), "non-zero flow id is valid");
    expectTrue(fuse::profiler::findFirstEventIndexByFlowId(0u) == fuse::profiler::kInvalidEventIndex,
               "findFirstEventIndexByFlowId rejects zero flow id");
    expectTrue(fuse::profiler::countEventsByFlowId(0u) == 0u,
               "countEventsByFlowId returns zero for zero flow id");
    expectTrue(!fuse::profiler::hasEventsWithFlowId(0u), "hasEventsWithFlowId false for zero flow id");

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

        FUSE_PROFILE_SCOPE("flow_lookup_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow_lookup_outer_io", outerFlowId);
            FUSE_PROFILE_SCOPE("flow_lookup_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow_lookup_inner_io", innerFlowId);
            FUSE_PROFILE_ASYNC_FLOW_END("flow_lookup_inner_io", innerFlowId);
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

void testTryFlowEventGuard() {
    expectTrue(fuse::profiler::findFirstEventIndexByName("flow_lookup_outer") == 0u,
               "name lookup ignores flow id field on scope events");

void testTryFindEventByNameGuard() {

    expectTrue(!fuse::profiler::tryFirstFlowEvent(0u, outEvent),
               "tryFirstFlowEvent false for zero flow id");
               "tryFirstFlowEvent clears output for zero flow id");

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

void testEventNameMatchesGuard() {

    expectTrue(!fuse::profiler::eventNameMatches(fuse::profiler::emptyProfileEvent(), "scope"),
               "eventNameMatches false for sentinel event");
    expectTrue(!fuse::profiler::eventNameMatches(fuse::profiler::emptyProfileEvent(), nullptr),
               "eventNameMatches false for null lookup name");

        FUSE_PROFILE_SCOPE("match_scope");

    const fuse::profiler::ProfileEvent& begin = fuse::profiler::eventAt(0);
    expectTrue(fuse::profiler::eventNameMatches(begin, "match_scope"),
               "eventNameMatches true for matching scope name");
    expectTrue(!fuse::profiler::eventNameMatches(begin, "other_scope"),
               "eventNameMatches false for mismatched scope name");
    expectTrue(!fuse::profiler::eventNameMatches(begin, ""),
               "eventNameMatches false for empty lookup name");

void testEmptyNameAttemptsDoNotAffectNameAndFlowLookup() {


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
               "tryFindFirstEventByName copies begin phase");
    expectTrue(fuse::profiler::tryFindLastEventByName("named_counter", outEvent),
               "tryFindLastEventByName true for counter sample");
               "tryFindLastEventByName copies counter phase");

void testTryFindFlowEventGuard() {

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

    const fuse::profiler::NestingAsyncFlowPreflight resetPreflight =
        fuse::profiler::preflightNestingAndAsyncFlow();
    expectTrue(resetPreflight.scopeNestingBalanced, "reset leaves scope nesting balanced");
    expectTrue(resetPreflight.flowNestingBalanced, "reset leaves flow nesting balanced");
    expectTrue(resetPreflight.canBeginScope(), "reset allows scope begin");
    expectTrue(resetPreflight.canBeginAsyncFlow(), "reset allows async flow begin");
    expectTrue(!resetPreflight.canEndAsyncFlow(), "reset blocks async flow end with no open flows");
    expectTrue(resetPreflight.isNestingHealthy(), "reset nesting state is healthy");

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

    const fuse::profiler::NestingAsyncFlowPreflight closedPreflight =
    expectTrue(closedPreflight.scopeNestingBalanced, "scope end restores balanced nesting");
    expectTrue(closedPreflight.flowNestingBalanced, "flow end restores balanced flow nesting");
    expectTrue(closedPreflight.isNestingHealthy(), "balanced trace reports healthy nesting");

void testPreflightNestingAndAsyncFlowCrossThreadHandoff() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_handoff_flow", flowId);

        FUSE_PROFILE_ASYNC_FLOW_END("preflight_handoff_flow", flowId);

    const fuse::profiler::NestingAsyncFlowPreflight preflight =
    expectTrue(preflight.flowDepthDetached, "preflight marks detached flow depth after handoff");
               "preflight marks cross-thread handoff pending");
    expectTrue(!preflight.isNestingHealthy(), "handoff leaves nesting unhealthy on begin thread");
    expectTrue(!preflight.canBeginAsyncFlow(),
               "handoff blocks new async flow begin on detached thread");
    expectTrue(!preflight.canEndAsyncFlow(),
               "handoff blocks async flow end when global open count is zero");

    const fuse::profiler::NestingAsyncFlowPreflight clearedPreflight =
    expectTrue(clearedPreflight.isNestingHealthy(), "reset clears unhealthy nesting state");
    expectTrue(clearedPreflight.canBeginAsyncFlow(), "reset re-enables async flow begin");

void testChromeTraceExportPreflightWithNameAndFlowLookup() {

        FUSE_PROFILE_SCOPE("preflight_lookup_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_lookup_flow", flowId);
        FUSE_PROFILE_COUNTER("preflight_lookup_counter", 6);
        FUSE_PROFILE_ASYNC_FLOW_END("preflight_lookup_flow", flowId);

    const fuse::profiler::ChromeTraceExportPreflight preflight =
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
    expectTrue(fuse::profiler::orphanAsyncFlowEndCount() == 2u, "orphan count tracks ignored finishes");
    expectTrue(fuse::profiler::hasOrphanAsyncFlowEnds(), "hasOrphanAsyncFlowEnds true after orphans");

    expectTrue(preflight.hasOrphanAsyncFlowEnds, "export preflight marks orphan flow ends");
    expectTrue(preflight.orphanAsyncFlowEndCount == 2u, "export preflight orphan count matches");

    expectTrue(fuse::profiler::orphanAsyncFlowEndCount() == 0u, "reset clears orphan count");

void testChromeTraceExportPreflightCanExportWithEvents() {

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
        FUSE_PROFILE_SCOPE("ignored_scope");
            FUSE_PROFILE_SCOPE("ignored_nested");

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled scopes record nothing");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "disabled scopes do not mutate nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u, "disabled scopes do not bump max nesting depth");

               "scope nesting depth resumes cleanly after re-enable");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "scope ends before reset check");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open flow survives scope exit until reset");
    expectTrue(fuse::profiler::hasEvents(), "events remain before reset");


    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "reset clears scope nesting depth");
    expectTrue(!fuse::profiler::isEventIndexValid(0u), "reset leaves isEventIndexValid false");
               "reset leaves empty chrome export");
}

void testDisabledAsyncFlowEndPreservesOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "flow begin sets hasOpenAsyncFlows");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin establishes open count");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_ASYNC_FLOW_END("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(),
               "disabled flow finish preserves hasOpenAsyncFlows");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "disabled flow finish preserves open async flow count");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled flow finish preserves thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 1u, "disabled flow finish records no new event");

    fuse::profiler::setEnabled(true);
               "re-enabled flow finish clears hasOpenAsyncFlows");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "re-enabled flow finish clears open async flow count");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "re-enabled flow finish restores thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow events recorded after disable guard");

void testCrossThreadFlowFinishSkipsWorkerFlowDepthPop() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "begin thread records flow depth");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "begin thread marks open async flow");

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
                   "worker thread starts with zero flow depth");
        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_flow", flowId);
                   "cross-thread flow finish does not underflow worker flow depth");
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

               "cross-thread flow finish clears global open flow state");
               "cross-thread flow finish clears global open count");
               "begin thread flow depth remains after cross-thread finish stub");

    fuse::profiler::reset();
               "reset clears begin-thread flow depth after cross-thread handoff");

void testDisabledScopeLiveNestingGuard() {

    {
        expectTrue(fuse::profiler::scopeNestingDepth() == 0u,
                   "disabled scope does not mutate scope nesting depth");
        expectTrue(fuse::profiler::nestingDepth() == 0u,
                   "disabled scope does not mutate nestingDepth alias");
               "disabled scope destructor does not underflow scope depth");

        FUSE_PROFILE_SCOPE("enabled_scope");
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u,
                   "scope nesting resumes cleanly after re-enable");
               "enabled scope restores depth after re-enable");

void testHasOpenAsyncFlowsIntrospection() {

    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset leaves hasOpenAsyncFlows false");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("open_flow", flowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "flow begin sets hasOpenAsyncFlows true");
    expectTrue(fuse::profiler::hasOpenAsyncFlows() == (fuse::profiler::openAsyncFlowCount() > 0u),
               "hasOpenAsyncFlows mirrors openAsyncFlowCount");

    FUSE_PROFILE_ASYNC_FLOW_END("open_flow", flowId);
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "flow end clears hasOpenAsyncFlows");

void testRingBufferCapacityGuard() {

    expectTrue(fuse::profiler::ringBufferCapacity() > 0u, "ring buffer capacity is non-zero");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringBufferCapacity(),
               "event count stays below ring capacity on empty buffer");

        FUSE_PROFILE_SCOPE("capacity_probe");

               "single scope stays below ring capacity");
    expectTrue(!fuse::profiler::isBufferFull(), "two events do not fill ring buffer");






    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "cross-thread flow begin sets hasOpenAsyncFlows");


               "cross-thread flow finish clears hasOpenAsyncFlows");


void testNestingDepthAliasesMatch() {

    expectTrue(fuse::profiler::nestingDepth() == fuse::profiler::scopeNestingDepth(),
               "nestingDepth matches scopeNestingDepth on reset");

        FUSE_PROFILE_SCOPE("alias_outer");
                   "nestingDepth matches scopeNestingDepth inside scope");
            FUSE_PROFILE_SCOPE("alias_inner");
            expectTrue(fuse::profiler::nestingDepth() == 2u, "nested scope depth is two");
            expectTrue(fuse::profiler::scopeNestingDepth() == 2u,
                       "scopeNestingDepth matches nested depth");
                   "nestingDepth matches scopeNestingDepth after inner scope");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "scope nesting depth clears after outer scope");
}

void testRingCapacityAndHasOpenAsyncFlowsGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::ringCapacity() == 4096u, "ring capacity exposes compile-time buffer size");
    expectTrue(fuse::profiler::isBufferEmpty(), "empty buffer guard true on reset");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringCapacity(),
               "event count stays below ring capacity on empty buffer");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false when no flows are open");

    {
        FUSE_PROFILE_SCOPE("capacity_probe");
    }

    expectTrue(!fuse::profiler::isBufferEmpty(), "recorded events clear isBufferEmpty");
    expectTrue(!fuse::profiler::isBufferFull(), "small trace does not fill ring buffer");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringCapacity(),
               "recorded events remain below ring capacity");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("tracked_flow", flowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true while flow is open");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open count tracks single flow begin");

    FUSE_PROFILE_ASYNC_FLOW_END("tracked_flow", flowId);
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after flow closes");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "open count clears after flow end");
}

void testEmptyProfileEventSentinel() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isValidProfileEvent(fuse::profiler::emptyProfileEvent()),
               "emptyProfileEvent is not a valid recorded event");
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

void testDisabledAsyncFlowEndPreservesOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin establishes open count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "flow begin marks async flow open");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_ASYNC_FLOW_END("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "disabled flow finish preserves open async flow count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(),
               "disabled flow finish preserves hasOpenAsyncFlows");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled flow finish preserves thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 1u, "disabled flow finish does not emit finish event");

    fuse::profiler::setEnabled(true);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "re-enabled flow finish clears open async flow count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "re-enabled flow finish clears hasOpenAsyncFlows");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "re-enabled flow finish restores thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow events recorded after disable guard");
}

void testCrossThreadFlowFinishSkipsWorkerFlowDepthPop() {

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "begin thread records flow depth");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "cross-thread flow begin marks flow open");

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
                   "worker thread starts with zero flow depth");
        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_flow", flowId);
                   "cross-thread flow finish does not underflow worker flow depth");
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

               "cross-thread flow finish clears global open count");
               "cross-thread flow finish clears hasOpenAsyncFlows");
               "begin thread flow depth remains after cross-thread finish stub");

    fuse::profiler::reset();
               "reset clears begin-thread flow depth after cross-thread handoff");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset clears buffer after cross-thread handoff");

void testDisabledProfilerSkipsSnapshotCounter() {

    fuse::profiler::beginFrame();
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_snapshot", 512);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_float_snapshot", 1.25);

    expectTrue(fuse::profiler::eventCount() == 0u,
               "disabled profiler skips snapshot_at_frame counter samples");
    expectTrue(fuse::profiler::isBufferEmpty(), "disabled snapshot counter leaves buffer empty");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "disabled snapshot counter export stays empty");

void testIsNonEmptyProfileNameGuard() {

    expectTrue(!fuse::profiler::isNonEmptyProfileName(nullptr), "null name is not non-empty");
    expectTrue(!fuse::profiler::isNonEmptyProfileName(""), "empty string is not non-empty");
    expectTrue(fuse::profiler::isNonEmptyProfileName("valid"), "non-empty name passes guard");

    const fuse::profiler::ProfilerRecordPreflight nullPreflight = fuse::profiler::preflightRecord(nullptr);
    expectTrue(!nullPreflight.name_valid, "preflight rejects null name");
    expectTrue(!nullPreflight.can_record_scope(), "preflight blocks null scope name");

    const fuse::profiler::ProfilerRecordPreflight emptyPreflight = fuse::profiler::preflightRecord("");
    expectTrue(!emptyPreflight.name_valid, "preflight rejects empty name");
    expectTrue(!emptyPreflight.can_record_counter(), "preflight blocks empty counter track");

    const fuse::profiler::ProfilerRecordPreflight validPreflight = fuse::profiler::preflightRecord("scope");
    expectTrue(validPreflight.profiler_enabled, "preflight sees enabled profiler");
    expectTrue(validPreflight.name_valid, "preflight accepts valid name");
    expectTrue(validPreflight.can_record_async_flow(), "preflight allows valid async flow name");

void testEmptyNameScopeFlowAndCounterGuards() {

    {
        fuse::profiler::ProfileScope emptyScope("");
    fuse::profiler::beginAsyncFlow("", 1u);
    fuse::profiler::endAsyncFlow("", 1u);
    fuse::profiler::sampleCounter("", 42);
    fuse::profiler::sampleCounterFloat("", 1.5);
    fuse::profiler::sampleCounterSnapshotAtFrame("", 7);
    fuse::profiler::sampleCounterFloatSnapshotAtFrame("", 0.25);

    expectTrue(fuse::profiler::eventCount() == 0u, "empty flow/counter names record nothing");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "empty scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "empty flow names do not mutate flow depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "empty flow names do not increment open count");
               "empty-name guard leaves export empty");

void testTryEventAtAndTryLastEvent() {

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt false on empty buffer");
    expectTrue(!fuse::profiler::tryLastEvent(outEvent), "tryLastEvent false on empty buffer");

        FUSE_PROFILE_SCOPE("try_scope");

    expectTrue(fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt succeeds for first event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin phase");
    expectTrue(std::string(outEvent.name) == "try_scope", "tryEventAt copies event name");
    expectTrue(!fuse::profiler::tryEventAt(99u, outEvent), "tryEventAt false for out-of-range index");

    fuse::profiler::ProfileEvent lastEvent{};
    expectTrue(fuse::profiler::tryLastEvent(lastEvent), "tryLastEvent succeeds after recording");
    expectTrue(lastEvent.phase == fuse::profiler::EventPhase::End, "tryLastEvent returns most recent end");
    expectTrue(std::string(lastEvent.name) == "try_scope", "tryLastEvent preserves scope name");

void testPreflightChromeTraceExport() {

    const fuse::profiler::ProfilerExportPreflight emptyPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!emptyPreflight.has_events, "empty export preflight has no events");
    expectTrue(!emptyPreflight.can_export(), "empty export preflight cannot export");
    expectTrue(emptyPreflight.event_count == 0u, "empty export preflight event count is zero");
    expectTrue(emptyPreflight.dropped_event_count == 0u, "empty export preflight dropped count is zero");
    expectTrue(!emptyPreflight.buffer_full, "empty export preflight buffer is not full");

        FUSE_PROFILE_SCOPE("export_preflight_scope");

    const fuse::profiler::ProfilerExportPreflight recordedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(recordedPreflight.has_events, "recorded export preflight has events");
    expectTrue(recordedPreflight.can_export(), "recorded export preflight can export");
    expectTrue(recordedPreflight.event_count == 2u, "recorded export preflight reports event count");
    expectTrue(fuse::profiler::hasExportableEvents(), "hasExportableEvents mirrors preflight");

void testDisabledRecordPreflight() {

    const fuse::profiler::ProfilerRecordPreflight disabledPreflight = fuse::profiler::preflightRecord("scope");
    expectTrue(!disabledPreflight.profiler_enabled, "disabled profiler preflight reports disabled");
    expectTrue(disabledPreflight.name_valid, "disabled profiler preflight still validates name");
    expectTrue(!disabledPreflight.can_record_scope(), "disabled profiler preflight blocks scope");
    expectTrue(!disabledPreflight.can_record_counter(), "disabled profiler preflight blocks counter");

void testRingCapacityAndDroppedEventCount() {

    expectTrue(fuse::profiler::ringCapacity() == fuse::profiler::kRingCapacity,
               "ringCapacity matches constexpr capacity");
    expectTrue(fuse::profiler::droppedEventCount() == 0u, "reset leaves dropped count at zero");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");

    for (fuse::u32 i = 0; i < fuse::profiler::kRingCapacity; ++i) {
        fuse::profiler::sampleCounter("overflow_track", static_cast<fuse::s64>(i));

    expectTrue(fuse::profiler::eventCount() == fuse::profiler::kRingCapacity,
               "event count caps at ring capacity");
    expectTrue(fuse::profiler::isBufferFull(), "buffer reports full at capacity");
    expectTrue(fuse::profiler::droppedEventCount() == 0u, "no drops until capacity is exceeded");

    fuse::profiler::sampleCounter("overflow_tail", 999);
               "event count stays capped after overflow");
    expectTrue(fuse::profiler::droppedEventCount() == 1u, "overflow increments dropped count");

    const fuse::profiler::ProfilerExportPreflight overflowPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(overflowPreflight.buffer_full, "overflow preflight reports full buffer");
    expectTrue(overflowPreflight.dropped_event_count == 1u, "overflow preflight reports dropped count");
    expectTrue(overflowPreflight.can_export(), "full buffer still has exportable events");

    expectTrue(fuse::profiler::droppedEventCount() == 0u, "reset clears dropped event count");

void testHasOpenAsyncFlowsGuard() {

    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset leaves hasOpenAsyncFlows false");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("open_flow", flowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true after begin");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open count matches hasOpenAsyncFlows");

    FUSE_PROFILE_ASYNC_FLOW_END("open_flow", flowId);
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after paired end");

void testIsValidProfileNamePreflight() {

    expectTrue(!fuse::profiler::isValidProfileName(nullptr), "null name fails profile name preflight");
    expectTrue(!fuse::profiler::isValidProfileName(""), "empty string fails profile name preflight");
    expectTrue(fuse::profiler::isValidProfileName("scope"), "non-empty name passes profile name preflight");

void testEmptyStringNameGuards() {


    fuse::profiler::beginAsyncFlow("", flowId);
    fuse::profiler::endAsyncFlow("", flowId);

    expectTrue(fuse::profiler::eventCount() == 0u, "empty-string names record nothing");
               "empty-string guard leaves export empty");

void testNestingBalancePreflights() {

    expectTrue(fuse::profiler::isScopeNestingBalanced(), "reset leaves scope nesting balanced");
    expectTrue(fuse::profiler::isFlowNestingBalanced(), "reset leaves flow nesting balanced");

        FUSE_PROFILE_SCOPE("balance_outer");
        expectTrue(!fuse::profiler::isScopeNestingBalanced(), "active scope marks nesting unbalanced");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("balance_flow", flowId);
        expectTrue(!fuse::profiler::isFlowNestingBalanced(), "open flow marks flow nesting unbalanced");
        expectTrue(fuse::profiler::hasOpenAsyncFlows(), "open flow sets hasOpenAsyncFlows");
        FUSE_PROFILE_ASYNC_FLOW_END("balance_flow", flowId);
        expectTrue(fuse::profiler::isFlowNestingBalanced(), "flow end restores flow nesting balance");
        expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "flow end clears hasOpenAsyncFlows");
    expectTrue(fuse::profiler::isScopeNestingBalanced(), "scope end restores scope nesting balance");

void testChromeTraceExportPreflight() {

    const fuse::profiler::ChromeTraceExportPreflight empty = fuse::profiler::preflightChromeTraceExport();
    expectTrue(empty.can_export, "empty buffer export preflight can export");
    expectTrue(empty.buffer_empty, "empty buffer preflight marks buffer empty");
    expectTrue(!empty.has_events, "empty buffer preflight has no events");
    expectTrue(empty.scope_nesting_balanced, "empty buffer preflight scope nesting balanced");
    expectTrue(empty.flow_nesting_balanced, "empty buffer preflight flow nesting balanced");
    expectTrue(!empty.has_open_async_flows, "empty buffer preflight has no open flows");
    expectTrue(empty.event_count == 0u, "empty buffer preflight event count is zero");
    expectTrue(empty.frame_index == 0u, "empty buffer preflight frame index is zero");

        FUSE_PROFILE_SCOPE("preflight_scope");

    const fuse::profiler::ChromeTraceExportPreflight recorded = fuse::profiler::preflightChromeTraceExport();
    expectTrue(recorded.can_export, "recorded buffer export preflight can export");
    expectTrue(recorded.has_events, "recorded buffer preflight has events");
    expectTrue(!recorded.buffer_empty, "recorded buffer preflight is not empty");
    expectTrue(recorded.event_count == 2u, "recorded buffer preflight reports event count");
    expectTrue(recorded.scope_nesting_balanced, "recorded buffer preflight scope nesting balanced");
    expectTrue(recorded.flow_nesting_balanced, "recorded buffer preflight flow nesting balanced");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_open_flow", flowId);
    const fuse::profiler::ChromeTraceExportPreflight openFlow = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!openFlow.can_export, "open async flow blocks export preflight");
    expectTrue(!openFlow.flow_nesting_balanced, "open async flow marks flow nesting unbalanced");
    expectTrue(openFlow.has_open_async_flows, "open async flow preflight reports open flows");
    FUSE_PROFILE_ASYNC_FLOW_END("preflight_open_flow", flowId);

void testSafeLookupStubs() {

    const fuse::profiler::ProfileEvent& sentinel = fuse::profiler::emptyProfileEvent();
    expectTrue(sentinel.name == nullptr, "emptyProfileEvent returns null-name sentinel");
    expectTrue(!fuse::profiler::isValidProfileEvent(sentinel),
               "emptyProfileEvent sentinel fails isValidProfileEvent");
    expectTrue(&fuse::profiler::eventAt(0) == &sentinel,
               "eventAt on empty buffer returns emptyProfileEvent sentinel");
    expectTrue(&fuse::profiler::lastEvent() == &sentinel,
               "lastEvent on empty buffer returns emptyProfileEvent sentinel");

    fuse::profiler::ProfileEvent out{};
    expectTrue(!fuse::profiler::tryEventAt(0, out), "tryEventAt fails on empty buffer");

        FUSE_PROFILE_SCOPE("lookup_scope");

    expectTrue(fuse::profiler::tryEventAt(0, out), "tryEventAt succeeds for valid index");
    expectTrue(out.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin event");
    expectTrue(std::string(out.name) == "lookup_scope", "tryEventAt preserves event name");
    expectTrue(!fuse::profiler::tryEventAt(99, out), "tryEventAt fails for out-of-range index");

void testRingCapacityIntrospection() {

    expectTrue(fuse::profiler::ringCapacity() == fuse::profiler::kRingEventCapacity,
               "ringCapacity matches kRingEventCapacity");
    expectTrue(fuse::profiler::kRingEventCapacity == 4096u, "kRingEventCapacity is 4096");
    expectTrue(!fuse::profiler::isBufferFull(), "fresh buffer is not full");
void testChromeExportPreflight() {

    const fuse::profiler::ChromeExportPreflight emptyPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(emptyPreflight.canExport, "empty preflight allows export");
    expectTrue(emptyPreflight.bufferEmpty, "empty preflight reports empty buffer");
    expectTrue(!emptyPreflight.hasEvents, "empty preflight reports no events");
    expectTrue(emptyPreflight.eventCount == 0u, "empty preflight reports zero event count");
    expectTrue(emptyPreflight.scopeNestingBalanced, "empty preflight reports balanced scope nesting");
    expectTrue(emptyPreflight.flowNestingBalanced, "empty preflight reports balanced flow nesting");
    expectTrue(!emptyPreflight.hasOpenAsyncFlows, "empty preflight reports no open async flows");

        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_flow", flowId);
        FUSE_PROFILE_COUNTER("preflight_counter", 9);

    const fuse::profiler::ChromeExportPreflight activePreflight = fuse::profiler::preflightChromeExport();
    expectTrue(activePreflight.canExport, "active preflight allows export");
    expectTrue(!activePreflight.bufferEmpty, "active preflight reports non-empty buffer");
    expectTrue(activePreflight.hasEvents, "active preflight reports events");
    expectTrue(activePreflight.eventCount >= 4u, "active preflight reports recorded event count");
    expectTrue(activePreflight.scopeNestingBalanced, "ended scope leaves nesting balanced in preflight");
    expectTrue(!activePreflight.flowNestingBalanced, "unmatched flow leaves flow nesting unbalanced in preflight");
    expectTrue(activePreflight.hasOpenAsyncFlows, "unmatched flow reports open async flows in preflight");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"preflight_scope\"") != std::string::npos,
               "preflight does not block chrome export of scope events");
    expectTrue(json.find("\"name\":\"preflight_counter\"") != std::string::npos,
               "preflight does not block chrome export of counter events");
}

void testEmptyNamePreflightGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::isEmptyProfilerName(nullptr), "null name is empty");
    expectTrue(fuse::profiler::isEmptyProfilerName(""), "empty string name is empty");
    expectTrue(!fuse::profiler::isEmptyProfilerName("valid"), "non-empty name is not empty");

    const fuse::profiler::ProfileNamePreflight nullPreflight = fuse::profiler::preflightProfileName(nullptr);
    expectTrue(nullPreflight.null_name, "preflight flags null name");
    expectTrue(!nullPreflight.empty_name, "preflight does not flag empty on null");
    expectTrue(!nullPreflight.canRecord(), "null name cannot record");
    expectTrue(nullPreflight.shouldSkip(), "null name should skip");

    const fuse::profiler::ProfileNamePreflight emptyPreflight = fuse::profiler::preflightProfileName("");
    expectTrue(!emptyPreflight.null_name, "preflight does not flag null on empty string");
    expectTrue(emptyPreflight.empty_name, "preflight flags empty string name");
    expectTrue(!emptyPreflight.canRecord(), "empty string name cannot record");
    expectTrue(emptyPreflight.shouldSkip(), "empty string name should skip");

    const fuse::profiler::ProfileNamePreflight validPreflight = fuse::profiler::preflightProfileName("scope");
    expectTrue(!validPreflight.null_name && !validPreflight.empty_name, "valid name passes preflight flags");
    expectTrue(validPreflight.canRecord(), "valid name can record");
    expectTrue(!validPreflight.shouldSkip(), "valid name should not skip");

    expectTrue(fuse::profiler::shouldSkipProfileScope(nullptr), "shouldSkipProfileScope true for null");
    expectTrue(fuse::profiler::shouldSkipCounterSample(""), "shouldSkipCounterSample true for empty");
    expectTrue(!fuse::profiler::shouldSkipProfileScope("ok"), "shouldSkipProfileScope false for valid name");
}

void testScopeNestingPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ScopeNestingPreflight initial = fuse::profiler::preflightScopeNesting();
    expectTrue(initial.current_depth == 0u, "preflight starts at zero depth");
    expectTrue(initial.max_depth_seen == 0u, "preflight starts with zero max depth");
    expectTrue(initial.canPush(), "preflight can push when profiler enabled");

    {
        FUSE_PROFILE_SCOPE("outer");
        const fuse::profiler::ScopeNestingPreflight nested = fuse::profiler::preflightScopeNesting();
        expectTrue(nested.current_depth == 1u, "preflight reflects active scope depth");
        expectTrue(nested.max_depth_seen == 1u, "preflight reflects max depth seen");
        expectTrue(nested.canPush(), "preflight can push inside active scope");
    }

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ScopeNestingPreflight disabled = fuse::profiler::preflightScopeNesting();
    expectTrue(disabled.profiler_disabled, "preflight flags disabled profiler");
    expectTrue(!disabled.canPush(), "preflight cannot push when profiler disabled");
}

void testAsyncFlowPreflights() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::AsyncFlowEndPreflight orphanPreflight = fuse::profiler::preflightAsyncFlowEnd("flow");
    expectTrue(orphanPreflight.orphan_end, "preflight flags orphan end with no open flows");
    expectTrue(!orphanPreflight.canEnd(), "orphan end cannot proceed");
    expectTrue(orphanPreflight.shouldSkip(), "orphan end should skip");
    expectTrue(fuse::profiler::shouldSkipAsyncFlowEnd("flow"), "shouldSkipAsyncFlowEnd true for orphan");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    const fuse::profiler::AsyncFlowBeginPreflight beginPreflight =
        fuse::profiler::preflightAsyncFlowBegin("vfs_load");
    expectTrue(beginPreflight.canBegin(), "valid begin preflight passes");
    expectTrue(!beginPreflight.shouldSkip(), "valid begin should not skip");
    expectTrue(!fuse::profiler::shouldSkipAsyncFlowBegin("vfs_load"), "shouldSkipAsyncFlowBegin false for valid");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("vfs_load", flowId);
    const fuse::profiler::AsyncFlowEndPreflight matchedPreflight = fuse::profiler::preflightAsyncFlowEnd("vfs_load");
    expectTrue(!matchedPreflight.orphan_end, "preflight clears orphan flag after begin");
    expectTrue(matchedPreflight.canEnd(), "matched end preflight passes");
    expectTrue(!fuse::profiler::shouldSkipAsyncFlowEnd("vfs_load"), "shouldSkipAsyncFlowEnd false when matched");

    const fuse::profiler::AsyncFlowBeginPreflight nullBegin = fuse::profiler::preflightAsyncFlowBegin(nullptr);
    expectTrue(nullBegin.null_name, "begin preflight flags null name");
    expectTrue(nullBegin.shouldSkip(), "null begin should skip");
    expectTrue(fuse::profiler::shouldSkipAsyncFlowBegin(nullptr), "shouldSkipAsyncFlowBegin true for null");

    FUSE_PROFILE_ASYNC_FLOW_END("vfs_load", flowId);
}

void testChromeTraceExportPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.would_emit_empty_trace, "empty buffer would emit empty trace");
    expectTrue(!emptyPreflight.has_events, "empty buffer has no events");
    expectTrue(emptyPreflight.shouldSkip(), "empty export should skip");
    expectTrue(emptyPreflight.canExport(), "export preflight always allows export");

    {
        FUSE_PROFILE_SCOPE("export_scope");
    }

    const fuse::profiler::ChromeTraceExportPreflight withEvents = fuse::profiler::preflightChromeTraceExport();
    expectTrue(withEvents.has_events, "preflight detects recorded events");
    expectTrue(!withEvents.would_emit_empty_trace, "non-empty buffer would emit trace");
    expectTrue(!withEvents.shouldSkip(), "non-empty export should not skip");
    expectTrue(withEvents.event_count >= 2u, "preflight reports event count");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("unmatched", flowId);
    const fuse::profiler::ChromeTraceExportPreflight unmatched = fuse::profiler::preflightChromeTraceExport();
    expectTrue(unmatched.has_unmatched_flows, "preflight flags unmatched async flows");
    expectTrue(unmatched.open_async_flow_count == 1u, "preflight reports open flow count");
}

void testSafeEventLookupStubs() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent out{};
    expectTrue(!fuse::profiler::tryEventAt(0u, out), "tryEventAt false on empty buffer");
    expectTrue(fuse::profiler::eventAtOrNull(0u) == nullptr, "eventAtOrNull null on empty buffer");
    expectTrue(fuse::profiler::eventAtOrNull(99u) == nullptr, "eventAtOrNull null when out of range");

    {
        FUSE_PROFILE_SCOPE("lookup_scope");
    }

    expectTrue(fuse::profiler::tryEventAt(0u, out), "tryEventAt true for first event");
    expectTrue(out.name != nullptr && std::string(out.name) == "lookup_scope",
               "tryEventAt copies scope begin name");
    expectTrue(out.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin phase");

    expectTrue(fuse::profiler::tryEventAt(1u, out), "tryEventAt true for second event");
    expectTrue(out.phase == fuse::profiler::EventPhase::End, "tryEventAt copies end phase");

    expectTrue(!fuse::profiler::tryEventAt(2u, out), "tryEventAt false past event count");

    const fuse::profiler::ProfileEvent* beginPtr = fuse::profiler::eventAtOrNull(0u);
    expectTrue(beginPtr != nullptr, "eventAtOrNull returns pointer for valid index");
    expectTrue(beginPtr->phase == fuse::profiler::EventPhase::Begin, "eventAtOrNull begin phase");
    expectTrue(fuse::profiler::eventAtOrNull(2u) == nullptr, "eventAtOrNull null past count");
}

void testRingBufferCapacityIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::ringBufferCapacity() == 4096u, "ring buffer capacity is 4096");
    expectTrue(fuse::profiler::remainingEventCapacity() == 4096u, "empty buffer has full capacity");

    {
        FUSE_PROFILE_SCOPE("capacity_probe");
    }

    expectTrue(fuse::profiler::remainingEventCapacity() == 4094u,
               "two events reduce remaining capacity");
    expectTrue(!fuse::profiler::isBufferFull(), "two events do not fill buffer");
}

void testEmptyStringNameGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isValidProfileName(nullptr), "isValidProfileName rejects null");
    expectTrue(!fuse::profiler::isValidProfileName(""), "isValidProfileName rejects empty string");

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

void testEmptyProfileEventSentinel() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isValidProfileEvent(fuse::profiler::emptyProfileEvent()),
               "emptyProfileEvent is not a valid recorded event");
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

void testRingCapacityAndHasOpenAsyncFlowsGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::ringCapacity() == 4096u, "ring capacity exposes compile-time buffer size");
    expectTrue(fuse::profiler::isBufferEmpty(), "empty buffer guard true on reset");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringCapacity(),
               "event count stays below ring capacity on empty buffer");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false when no flows are open");

    {
        FUSE_PROFILE_SCOPE("capacity_probe");
    }

    expectTrue(!fuse::profiler::isBufferEmpty(), "recorded events clear isBufferEmpty");
    expectTrue(!fuse::profiler::isBufferFull(), "small trace does not fill ring buffer");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringCapacity(),
               "recorded events remain below ring capacity");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("tracked_flow", flowId);
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true while flow is open");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open count tracks single flow begin");

    FUSE_PROFILE_ASYNC_FLOW_END("tracked_flow", flowId);
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after flow closes");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "open count clears after flow end");
}

void testChromeTraceExportPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.canExport(), "preflight allows export when profiler is enabled");
    expectTrue(emptyPreflight.profilerEnabled, "preflight reports profiler enabled");
    expectTrue(emptyPreflight.bufferEmpty, "preflight reports empty buffer on reset");
    expectTrue(!emptyPreflight.hasExportableEvents, "preflight reports no exportable events on reset");
    expectTrue(emptyPreflight.exportableEventCount == 0u, "preflight exportable count is zero on reset");
    expectTrue(emptyPreflight.scopeNestingBalanced, "preflight reports balanced scope nesting on reset");
    expectTrue(emptyPreflight.flowNestingBalanced, "preflight reports balanced flow nesting on reset");
    expectTrue(!emptyPreflight.hasOpenAsyncFlows, "preflight reports no open flows on reset");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("preflight_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_flow", flowId);
        FUSE_PROFILE_COUNTER("preflight_counter", 9);
    }

    const fuse::profiler::ChromeTraceExportPreflight activePreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(activePreflight.canExport(), "preflight allows export with recorded events");
    expectTrue(!activePreflight.bufferEmpty, "preflight reports non-empty buffer");
    expectTrue(activePreflight.hasExportableEvents, "preflight reports exportable events");
    expectTrue(activePreflight.exportableEventCount == activePreflight.eventCount,
               "preflight exportable count matches event count for valid names");
    expectTrue(activePreflight.frameIndex == 1u, "preflight reports current frame index");
    expectTrue(activePreflight.scopeNestingBalanced,
               "preflight reports balanced scope nesting after scope ends");
    expectTrue(!activePreflight.flowNestingBalanced,
               "preflight reports unbalanced flow nesting with unmatched begin");
    expectTrue(activePreflight.hasOpenAsyncFlows, "preflight reports open async flows");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ChromeTraceExportPreflight disabledPreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(!disabledPreflight.canExport(), "preflight blocks export when profiler is disabled");
    expectTrue(!disabledPreflight.profilerEnabled, "preflight reports profiler disabled");
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

void testDisabledAsyncFlowEndPreservesOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin establishes open count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "flow begin marks async flow open");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_ASYNC_FLOW_END("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "disabled flow finish preserves open async flow count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(),
               "disabled flow finish preserves hasOpenAsyncFlows");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled flow finish preserves thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 1u, "disabled flow finish does not emit finish event");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_END("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "re-enabled flow finish clears open async flow count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "re-enabled flow finish clears hasOpenAsyncFlows");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "re-enabled flow finish restores thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow events recorded after disable guard");
}

void testCrossThreadFlowFinishSkipsWorkerFlowDepthPop() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "begin thread records flow depth");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "cross-thread flow begin marks flow open");

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
        expectTrue(fuse::profiler::flowNestingDepth() == 0u,
                   "worker thread starts with zero flow depth");
        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_flow", flowId);
        expectTrue(fuse::profiler::flowNestingDepth() == 0u,
                   "cross-thread flow finish does not underflow worker flow depth");
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "cross-thread flow finish clears global open count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "cross-thread flow finish clears hasOpenAsyncFlows");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "begin thread flow depth remains after cross-thread finish stub");

    fuse::profiler::reset();
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "reset clears begin-thread flow depth after cross-thread handoff");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset clears buffer after cross-thread handoff");
}

void testDisabledProfilerSkipsSnapshotCounter() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_snapshot", 512);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_float_snapshot", 1.25);

    expectTrue(fuse::profiler::eventCount() == 0u,
               "disabled profiler skips snapshot_at_frame counter samples");
    expectTrue(fuse::profiler::isBufferEmpty(), "disabled snapshot counter leaves buffer empty");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "disabled snapshot counter export stays empty");
}

void testProfileNamePreflight() {
    resetState();

    const fuse::profiler::ProfileNamePreflight nullPreflight = fuse::profiler::preflightProfileName(nullptr);
    expectTrue(nullPreflight.null_name, "null name preflight marks null_name");
    expectTrue(!nullPreflight.empty_name, "null name preflight clears empty_name");
    expectTrue(!nullPreflight.can_record(), "null name preflight cannot record");
    expectTrue(nullPreflight.should_skip(), "null name preflight should skip");

    const fuse::profiler::ProfileNamePreflight emptyPreflight = fuse::profiler::preflightProfileName("");
    expectTrue(!emptyPreflight.null_name, "empty string preflight clears null_name");
    expectTrue(emptyPreflight.empty_name, "empty string preflight marks empty_name");
    expectTrue(!emptyPreflight.can_record(), "empty string preflight cannot record");

    const fuse::profiler::ProfileNamePreflight validPreflight = fuse::profiler::preflightProfileName("valid");
    expectTrue(!validPreflight.null_name, "valid name preflight clears null_name");
    expectTrue(!validPreflight.empty_name, "valid name preflight clears empty_name");
    expectTrue(validPreflight.can_record(), "valid name preflight can record");
    expectTrue(!validPreflight.should_skip(), "valid name preflight should not skip");
    expectTrue(fuse::profiler::isUsableProfileName("scope"), "isUsableProfileName accepts non-empty");
    expectTrue(!fuse::profiler::isUsableProfileName(nullptr), "isUsableProfileName rejects null");
    expectTrue(!fuse::profiler::isUsableProfileName(""), "isUsableProfileName rejects empty string");
}

void testScopePreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ScopePreflight nullPreflight = fuse::profiler::preflightScope(nullptr);
    expectTrue(nullPreflight.null_name, "scope preflight marks null name");
    expectTrue(!nullPreflight.can_enter(), "scope preflight rejects null name");

    const fuse::profiler::ScopePreflight emptyPreflight = fuse::profiler::preflightScope("");
    expectTrue(emptyPreflight.empty_name, "scope preflight marks empty name");
    expectTrue(!emptyPreflight.can_enter(), "scope preflight rejects empty name");

    const fuse::profiler::ScopePreflight validPreflight = fuse::profiler::preflightScope("valid_scope");
    expectTrue(validPreflight.can_enter(), "scope preflight accepts valid name while enabled");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ScopePreflight disabledPreflight = fuse::profiler::preflightScope("valid_scope");
    expectTrue(disabledPreflight.profiler_disabled, "scope preflight marks disabled profiler");
    expectTrue(!disabledPreflight.can_enter(), "scope preflight rejects while disabled");
}

void testScopeNestingPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ScopeNestingPreflight rootPreflight = fuse::profiler::preflightScopeNesting();
    expectTrue(rootPreflight.is_at_root(), "nesting preflight at root after reset");
    expectTrue(rootPreflight.current_depth == 0u, "nesting preflight reports zero current depth");
    expectTrue(rootPreflight.max_observed_depth == 0u, "nesting preflight reports zero max depth");

    {
        FUSE_PROFILE_SCOPE("nesting_outer");
        const fuse::profiler::ScopeNestingPreflight outerPreflight = fuse::profiler::preflightScopeNesting();
        expectTrue(!outerPreflight.is_at_root(), "nesting preflight not at root inside scope");
        expectTrue(outerPreflight.current_depth == 1u, "nesting preflight reports depth 1 in outer scope");
        {
            FUSE_PROFILE_SCOPE("nesting_inner");
            const fuse::profiler::ScopeNestingPreflight innerPreflight = fuse::profiler::preflightScopeNesting();
            expectTrue(innerPreflight.current_depth == 2u, "nesting preflight reports depth 2 in inner scope");
            expectTrue(innerPreflight.max_observed_depth == 2u, "nesting preflight tracks max observed depth");
        }
    }

    const fuse::profiler::ScopeNestingPreflight afterPreflight = fuse::profiler::preflightScopeNesting();
    expectTrue(afterPreflight.is_at_root(), "nesting preflight returns to root after scopes end");
}

void testAsyncFlowPreflights() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::AsyncFlowBeginPreflight nullBegin = fuse::profiler::preflightBeginAsyncFlow(nullptr);
    expectTrue(nullBegin.null_name, "flow begin preflight marks null name");
    expectTrue(!nullBegin.can_begin(), "flow begin preflight rejects null name");

    const fuse::profiler::AsyncFlowBeginPreflight emptyBegin = fuse::profiler::preflightBeginAsyncFlow("");
    expectTrue(emptyBegin.empty_name, "flow begin preflight marks empty name");
    expectTrue(!emptyBegin.can_begin(), "flow begin preflight rejects empty name");

    const fuse::profiler::AsyncFlowEndPreflight orphanEnd = fuse::profiler::preflightEndAsyncFlow("orphan");
    expectTrue(orphanEnd.no_open_flows, "flow end preflight marks no open flows");
    expectTrue(orphanEnd.would_orphan(), "flow end preflight would orphan without begin");
    expectTrue(!orphanEnd.can_end(), "flow end preflight rejects orphan finish");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);
    const fuse::profiler::AsyncFlowEndPreflight pairedEnd = fuse::profiler::preflightEndAsyncFlow("paired_flow");
    expectTrue(!pairedEnd.no_open_flows, "flow end preflight clears no_open_flows after begin");
    expectTrue(pairedEnd.open_flow_count == 1u, "flow end preflight reports open flow count");
    expectTrue(pairedEnd.can_end(), "flow end preflight accepts paired finish");
    FUSE_PROFILE_ASYNC_FLOW_END("paired_flow", flowId);

    fuse::profiler::setEnabled(false);
    const fuse::profiler::AsyncFlowBeginPreflight disabledBegin = fuse::profiler::preflightBeginAsyncFlow("flow");
    expectTrue(disabledBegin.profiler_disabled, "flow begin preflight marks disabled profiler");
    expectTrue(!disabledBegin.can_begin(), "flow begin preflight rejects while disabled");
}

void testCounterSamplePreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::CounterSamplePreflight nullPreflight = fuse::profiler::preflightCounterSample(nullptr);
    expectTrue(nullPreflight.null_name, "counter preflight marks null track");
    expectTrue(!nullPreflight.can_sample(), "counter preflight rejects null track");

    const fuse::profiler::CounterSamplePreflight emptyPreflight = fuse::profiler::preflightCounterSample("");
    expectTrue(emptyPreflight.empty_name, "counter preflight marks empty track");
    expectTrue(!emptyPreflight.can_sample(), "counter preflight rejects empty track");

    const fuse::profiler::CounterSamplePreflight validPreflight = fuse::profiler::preflightCounterSample("budget");
    expectTrue(validPreflight.can_sample(), "counter preflight accepts valid track while enabled");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::CounterSamplePreflight disabledPreflight = fuse::profiler::preflightCounterSample("budget");
    expectTrue(disabledPreflight.profiler_disabled, "counter preflight marks disabled profiler");
    expectTrue(!disabledPreflight.can_sample(), "counter preflight rejects while disabled");
}

void testChromeTraceExportPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.exportable(), "export preflight always exportable");
    expectTrue(emptyPreflight.buffer_empty, "export preflight marks empty buffer");
    expectTrue(!emptyPreflight.will_emit_events(), "export preflight will not emit on empty buffer");
    expectTrue(emptyPreflight.frame_index == 0u, "export preflight reports frame index");

    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("export_scope");
        FUSE_PROFILE_COUNTER("export_counter", 5);
    }

    const fuse::profiler::ChromeTraceExportPreflight populatedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!populatedPreflight.buffer_empty, "export preflight clears buffer_empty after recording");
    expectTrue(populatedPreflight.event_count == 3u, "export preflight reports event count");
    expectTrue(populatedPreflight.will_emit_events(), "export preflight will emit events");
    expectTrue(populatedPreflight.null_name_skip_count == 0u, "export preflight reports zero null-name skips");
    expectTrue(populatedPreflight.frame_index == 1u, "export preflight reflects beginFrame count");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[") != std::string::npos,
               "export preflight aligns with callable export");
}

void testEventLookupPreflightAndTryEventAt() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::EventLookupPreflight emptyPreflight = fuse::profiler::preflightEventLookup(0u);
    expectTrue(emptyPreflight.buffer_empty, "lookup preflight marks empty buffer");
    expectTrue(emptyPreflight.out_of_range, "lookup preflight marks out of range on empty buffer");
    expectTrue(emptyPreflight.should_use_sentinel(), "lookup preflight should use sentinel on empty buffer");
    expectTrue(!fuse::profiler::canLookupEventAt(0u), "canLookupEventAt false on empty buffer");

    const fuse::profiler::ProfileEvent* lookupOut = nullptr;
    expectTrue(!fuse::profiler::tryEventAt(0u, lookupOut), "tryEventAt false on empty buffer");
    expectTrue(lookupOut != nullptr && lookupOut->name == nullptr, "tryEventAt returns sentinel on empty buffer");

    {
        FUSE_PROFILE_SCOPE("lookup_scope");
    }

    const fuse::profiler::EventLookupPreflight validPreflight = fuse::profiler::preflightEventLookup(0u);
    expectTrue(!validPreflight.buffer_empty, "lookup preflight clears buffer_empty after recording");
    expectTrue(!validPreflight.out_of_range, "lookup preflight accepts in-range index");
    expectTrue(validPreflight.can_lookup(), "lookup preflight can lookup first event");
    expectTrue(fuse::profiler::canLookupEventAt(0u), "canLookupEventAt true for first event");
    expectTrue(fuse::profiler::canLookupEventAt(1u), "canLookupEventAt true for last event");
    expectTrue(!fuse::profiler::canLookupEventAt(2u), "canLookupEventAt false past event count");

    const fuse::profiler::EventLookupPreflight oobPreflight = fuse::profiler::preflightEventLookup(99u);
    expectTrue(oobPreflight.out_of_range, "lookup preflight marks out-of-range index");
    expectTrue(!oobPreflight.can_lookup(), "lookup preflight rejects out-of-range index");

    expectTrue(fuse::profiler::tryEventAt(0u, lookupOut), "tryEventAt succeeds for valid index");
    expectTrue(lookupOut != nullptr && lookupOut->name != nullptr, "tryEventAt returns valid event pointer");
    expectTrue(std::string(lookupOut->name) == "lookup_scope", "tryEventAt preserves event name");
    expectTrue(lookupOut->phase == fuse::profiler::EventPhase::Begin, "tryEventAt returns begin event");

    expectTrue(!fuse::profiler::tryEventAt(99u, lookupOut), "tryEventAt false for out-of-range index");
    expectTrue(lookupOut != nullptr && lookupOut->name == nullptr, "tryEventAt returns sentinel for out-of-range");
}

void testIsValidProfileNameGuard() {
    resetState();

    expectTrue(!fuse::profiler::isValidProfileName(nullptr), "null name is invalid");
    expectTrue(!fuse::profiler::isValidProfileName(""), "empty string name is invalid");
    expectTrue(fuse::profiler::isValidProfileName("scope"), "non-empty name is valid");
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
    expectTrue(fuse::profiler::nestingDepth() == 0u, "empty scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "empty flow name does not mutate flow depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "empty flow name does not mutate open count");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "empty-string guard leaves export empty");
}

void testTryEventAtSafeLookup() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ProfileEvent* outEvent = nullptr;
    expectTrue(!fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt fails on empty buffer");
    expectTrue(outEvent != nullptr, "tryEventAt sets out pointer on failure");
    expectTrue(outEvent->name == nullptr, "tryEventAt failure returns empty sentinel");
    expectTrue(!fuse::profiler::isValidProfileEvent(*outEvent),
               "tryEventAt sentinel is not a valid profile event");

    {
        FUSE_PROFILE_SCOPE("lookup_scope");
    }

    expectTrue(fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt succeeds for first event");
    expectTrue(outEvent != nullptr && outEvent->name != nullptr, "tryEventAt returns recorded event");
    expectTrue(outEvent->phase == fuse::profiler::EventPhase::Begin, "tryEventAt preserves event phase");
    expectTrue(std::string(outEvent->name) == "lookup_scope", "tryEventAt preserves event name");

    expectTrue(fuse::profiler::tryEventAt(1u, outEvent), "tryEventAt succeeds for last event");
    expectTrue(outEvent->phase == fuse::profiler::EventPhase::End, "tryEventAt returns scope end");

    expectTrue(!fuse::profiler::tryEventAt(2u, outEvent), "tryEventAt fails past event count");
    expectTrue(outEvent->name == nullptr, "tryEventAt past count returns sentinel");
}

void testPreflightGuardState() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ProfilerGuardPreflight resetPreflight = fuse::profiler::preflightGuardState();
    expectTrue(!resetPreflight.hasOpenScopes, "reset preflight has no open scopes");
    expectTrue(!resetPreflight.hasOpenAsyncFlows, "reset preflight has no open async flows");
    expectTrue(!resetPreflight.canEndAsyncFlow, "reset preflight cannot end async flow");
    expectTrue(resetPreflight.scopeDepth == 0u, "reset preflight scope depth is zero");
    expectTrue(resetPreflight.flowDepth == 0u, "reset preflight flow depth is zero");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("preflight_outer");
        const fuse::profiler::ProfilerGuardPreflight scopedPreflight = fuse::profiler::preflightGuardState();
        expectTrue(scopedPreflight.hasOpenScopes, "scoped preflight marks open scopes");
        expectTrue(scopedPreflight.scopeDepth == 1u, "scoped preflight reports scope depth");
        expectTrue(fuse::profiler::hasOpenScopes(), "hasOpenScopes mirrors preflight");

        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_flow", flowId);
        const fuse::profiler::ProfilerGuardPreflight flowPreflight = fuse::profiler::preflightGuardState();
        expectTrue(flowPreflight.hasOpenAsyncFlows, "flow preflight marks open async flows");
        expectTrue(flowPreflight.canEndAsyncFlow, "flow preflight allows async finish");
        expectTrue(flowPreflight.openAsyncFlows == 1u, "flow preflight tracks open count");
        expectTrue(flowPreflight.flowDepth == 1u, "flow preflight reports flow depth");
        expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows mirrors preflight");
        expectTrue(flowPreflight.hasUnmatchedAsyncFlows(), "flow preflight marks unmatched flows");

        FUSE_PROFILE_ASYNC_FLOW_END("preflight_flow", flowId);
    }

    const fuse::profiler::ProfilerGuardPreflight clearedPreflight = fuse::profiler::preflightGuardState();
    expectTrue(!clearedPreflight.hasOpenScopes, "post-scope preflight clears open scopes");
    expectTrue(!clearedPreflight.hasOpenAsyncFlows, "post-flow preflight clears open async flows");
    expectTrue(clearedPreflight.maxScopeDepth >= 1u, "preflight retains max scope depth");
    expectTrue(clearedPreflight.maxFlowDepth >= 1u, "preflight retains max flow depth");
}

void testPreflightChromeTraceExport() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.canExport, "empty export preflight allows export");
    expectTrue(emptyPreflight.bufferEmpty, "empty export preflight marks empty buffer");
    expectTrue(!emptyPreflight.hasEventsToExport(), "empty export preflight has no events");
    expectTrue(emptyPreflight.frameIndex == 0u, "empty export preflight reports frame index");
    expectTrue(!emptyPreflight.hasOpenScopes, "empty export preflight has no open scopes");
    expectTrue(!emptyPreflight.hasUnmatchedAsyncFlows, "empty export preflight has no unmatched flows");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    fuse::profiler::beginFrame();
    fuse::profiler::ChromeTraceExportPreflight unmatchedPreflight{};
    {
        FUSE_PROFILE_SCOPE("export_preflight_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("export_preflight_flow", flowId);
        unmatchedPreflight = fuse::profiler::preflightChromeTraceExport();
        expectTrue(unmatchedPreflight.hasOpenScopes, "unmatched export preflight sees open scope");
    }

    expectTrue(unmatchedPreflight.canExport, "unmatched-flow export preflight still allows export");
    expectTrue(!unmatchedPreflight.bufferEmpty, "unmatched export preflight sees recorded events");
    expectTrue(unmatchedPreflight.hasEventsToExport(), "unmatched export preflight has events");
    expectTrue(unmatchedPreflight.hasOpenAsyncFlows, "unmatched export preflight sees open flow");
    expectTrue(unmatchedPreflight.hasUnmatchedAsyncFlows,
               "unmatched export preflight flags unmatched async flow");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"export_preflight_scope\"") != std::string::npos,
               "preflight export still emits scope events");
    expectTrue(json.find("\"name\":\"export_preflight_flow\"") != std::string::npos,
               "preflight export still emits flow events");
}

void testIsValidProfileNameGuard() {
    resetState();

    expectTrue(!fuse::profiler::isValidProfileName(nullptr), "null name is invalid");
    expectTrue(!fuse::profiler::isValidProfileName(""), "empty string name is invalid");
    expectTrue(fuse::profiler::isValidProfileName("scope"), "non-empty name is valid");
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
    expectTrue(fuse::profiler::nestingDepth() == 0u, "empty scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "empty flow name does not mutate flow depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "empty flow name does not mutate open count");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "empty-string guard leaves export empty");
}

void testTryEventAtSafeLookup() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ProfileEvent* outEvent = nullptr;
    expectTrue(!fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt fails on empty buffer");
    expectTrue(outEvent != nullptr, "tryEventAt sets out pointer on failure");
    expectTrue(outEvent->name == nullptr, "tryEventAt failure returns empty sentinel");
    expectTrue(!fuse::profiler::isValidProfileEvent(*outEvent),
               "tryEventAt sentinel is not a valid profile event");

    {
        FUSE_PROFILE_SCOPE("lookup_scope");
    }

    expectTrue(fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt succeeds for first event");
    expectTrue(outEvent != nullptr && outEvent->name != nullptr, "tryEventAt returns recorded event");
    expectTrue(outEvent->phase == fuse::profiler::EventPhase::Begin, "tryEventAt preserves event phase");
    expectTrue(std::string(outEvent->name) == "lookup_scope", "tryEventAt preserves event name");

    expectTrue(fuse::profiler::tryEventAt(1u, outEvent), "tryEventAt succeeds for last event");
    expectTrue(outEvent->phase == fuse::profiler::EventPhase::End, "tryEventAt returns scope end");

    expectTrue(!fuse::profiler::tryEventAt(2u, outEvent), "tryEventAt fails past event count");
    expectTrue(outEvent->name == nullptr, "tryEventAt past count returns sentinel");
}

void testPreflightGuardState() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ProfilerGuardPreflight resetPreflight = fuse::profiler::preflightGuardState();
    expectTrue(!resetPreflight.hasOpenScopes, "reset preflight has no open scopes");
    expectTrue(!resetPreflight.hasOpenAsyncFlows, "reset preflight has no open async flows");
    expectTrue(!resetPreflight.canEndAsyncFlow, "reset preflight cannot end async flow");
    expectTrue(resetPreflight.scopeDepth == 0u, "reset preflight scope depth is zero");
    expectTrue(resetPreflight.flowDepth == 0u, "reset preflight flow depth is zero");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("preflight_outer");
        const fuse::profiler::ProfilerGuardPreflight scopedPreflight = fuse::profiler::preflightGuardState();
        expectTrue(scopedPreflight.hasOpenScopes, "scoped preflight marks open scopes");
        expectTrue(scopedPreflight.scopeDepth == 1u, "scoped preflight reports scope depth");
        expectTrue(fuse::profiler::hasOpenScopes(), "hasOpenScopes mirrors preflight");

        FUSE_PROFILE_ASYNC_FLOW_BEGIN("preflight_flow", flowId);
        const fuse::profiler::ProfilerGuardPreflight flowPreflight = fuse::profiler::preflightGuardState();
        expectTrue(flowPreflight.hasOpenAsyncFlows, "flow preflight marks open async flows");
        expectTrue(flowPreflight.canEndAsyncFlow, "flow preflight allows async finish");
        expectTrue(flowPreflight.openAsyncFlows == 1u, "flow preflight tracks open count");
        expectTrue(flowPreflight.flowDepth == 1u, "flow preflight reports flow depth");
        expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows mirrors preflight");
        expectTrue(flowPreflight.hasUnmatchedAsyncFlows(), "flow preflight marks unmatched flows");

        FUSE_PROFILE_ASYNC_FLOW_END("preflight_flow", flowId);
    }

    const fuse::profiler::ProfilerGuardPreflight clearedPreflight = fuse::profiler::preflightGuardState();
    expectTrue(!clearedPreflight.hasOpenScopes, "post-scope preflight clears open scopes");
    expectTrue(!clearedPreflight.hasOpenAsyncFlows, "post-flow preflight clears open async flows");
    expectTrue(clearedPreflight.maxScopeDepth >= 1u, "preflight retains max scope depth");
    expectTrue(clearedPreflight.maxFlowDepth >= 1u, "preflight retains max flow depth");
}

void testPreflightChromeTraceExport() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.canExport, "empty export preflight allows export");
    expectTrue(emptyPreflight.bufferEmpty, "empty export preflight marks empty buffer");
    expectTrue(!emptyPreflight.hasEventsToExport(), "empty export preflight has no events");
    expectTrue(emptyPreflight.frameIndex == 0u, "empty export preflight reports frame index");
    expectTrue(!emptyPreflight.hasOpenScopes, "empty export preflight has no open scopes");
    expectTrue(!emptyPreflight.hasUnmatchedAsyncFlows, "empty export preflight has no unmatched flows");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    fuse::profiler::beginFrame();
    fuse::profiler::ChromeTraceExportPreflight unmatchedPreflight{};
    {
        FUSE_PROFILE_SCOPE("export_preflight_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("export_preflight_flow", flowId);
        unmatchedPreflight = fuse::profiler::preflightChromeTraceExport();
        expectTrue(unmatchedPreflight.hasOpenScopes, "unmatched export preflight sees open scope");
    }

    expectTrue(unmatchedPreflight.canExport, "unmatched-flow export preflight still allows export");
    expectTrue(!unmatchedPreflight.bufferEmpty, "unmatched export preflight sees recorded events");
    expectTrue(unmatchedPreflight.hasEventsToExport(), "unmatched export preflight has events");
    expectTrue(unmatchedPreflight.hasOpenAsyncFlows, "unmatched export preflight sees open flow");
    expectTrue(unmatchedPreflight.hasUnmatchedAsyncFlows,
               "unmatched export preflight flags unmatched async flow");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"export_preflight_scope\"") != std::string::npos,
               "preflight export still emits scope events");
    expectTrue(json.find("\"name\":\"export_preflight_flow\"") != std::string::npos,
               "preflight export still emits flow events");
}

void testProfileNamePreflight() {
    resetState();

    const fuse::profiler::ProfileNamePreflight nullPreflight = fuse::profiler::preflightProfileName(nullptr);
    expectTrue(nullPreflight.null_name, "null name preflight marks null_name");
    expectTrue(!nullPreflight.empty_name, "null name preflight clears empty_name");
    expectTrue(!nullPreflight.can_record(), "null name preflight cannot record");
    expectTrue(nullPreflight.should_skip(), "null name preflight should skip");

    const fuse::profiler::ProfileNamePreflight emptyPreflight = fuse::profiler::preflightProfileName("");
    expectTrue(!emptyPreflight.null_name, "empty string preflight clears null_name");
    expectTrue(emptyPreflight.empty_name, "empty string preflight marks empty_name");
    expectTrue(!emptyPreflight.can_record(), "empty string preflight cannot record");

    const fuse::profiler::ProfileNamePreflight validPreflight = fuse::profiler::preflightProfileName("valid");
    expectTrue(!validPreflight.null_name, "valid name preflight clears null_name");
    expectTrue(!validPreflight.empty_name, "valid name preflight clears empty_name");
    expectTrue(validPreflight.can_record(), "valid name preflight can record");
    expectTrue(!validPreflight.should_skip(), "valid name preflight should not skip");
    expectTrue(fuse::profiler::isUsableProfileName("scope"), "isUsableProfileName accepts non-empty");
    expectTrue(!fuse::profiler::isUsableProfileName(nullptr), "isUsableProfileName rejects null");
    expectTrue(!fuse::profiler::isUsableProfileName(""), "isUsableProfileName rejects empty string");
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
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "empty scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "empty flow names do not mutate flow depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "empty flow names do not leave open async flows");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "empty-string guard leaves export empty");
}

void testScopePreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ScopePreflight nullPreflight = fuse::profiler::preflightScope(nullptr);
    expectTrue(nullPreflight.null_name, "scope preflight marks null name");
    expectTrue(!nullPreflight.can_enter(), "scope preflight rejects null name");

    const fuse::profiler::ScopePreflight emptyPreflight = fuse::profiler::preflightScope("");
    expectTrue(emptyPreflight.empty_name, "scope preflight marks empty name");
    expectTrue(!emptyPreflight.can_enter(), "scope preflight rejects empty name");

    const fuse::profiler::ScopePreflight validPreflight = fuse::profiler::preflightScope("valid_scope");
    expectTrue(validPreflight.can_enter(), "scope preflight accepts valid name while enabled");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ScopePreflight disabledPreflight = fuse::profiler::preflightScope("valid_scope");
    expectTrue(disabledPreflight.profiler_disabled, "scope preflight marks disabled profiler");
    expectTrue(!disabledPreflight.can_enter(), "scope preflight rejects while disabled");
}

void testScopeNestingPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ScopeNestingPreflight rootPreflight = fuse::profiler::preflightScopeNesting();
    expectTrue(rootPreflight.is_at_root(), "nesting preflight at root after reset");
    expectTrue(rootPreflight.current_depth == 0u, "nesting preflight reports zero current depth");
    expectTrue(rootPreflight.max_observed_depth == 0u, "nesting preflight reports zero max depth");

    {
        FUSE_PROFILE_SCOPE("nesting_outer");
        const fuse::profiler::ScopeNestingPreflight outerPreflight = fuse::profiler::preflightScopeNesting();
        expectTrue(!outerPreflight.is_at_root(), "nesting preflight not at root inside scope");
        expectTrue(outerPreflight.current_depth == 1u, "nesting preflight reports depth 1 in outer scope");
        {
            FUSE_PROFILE_SCOPE("nesting_inner");
            const fuse::profiler::ScopeNestingPreflight innerPreflight = fuse::profiler::preflightScopeNesting();
            expectTrue(innerPreflight.current_depth == 2u, "nesting preflight reports depth 2 in inner scope");
            expectTrue(innerPreflight.max_observed_depth == 2u, "nesting preflight tracks max observed depth");
        }
    }

    const fuse::profiler::ScopeNestingPreflight afterPreflight = fuse::profiler::preflightScopeNesting();
    expectTrue(afterPreflight.is_at_root(), "nesting preflight returns to root after scopes end");
}

void testAsyncFlowPreflights() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::AsyncFlowBeginPreflight nullBegin = fuse::profiler::preflightBeginAsyncFlow(nullptr);
    expectTrue(nullBegin.null_name, "flow begin preflight marks null name");
    expectTrue(!nullBegin.can_begin(), "flow begin preflight rejects null name");

    const fuse::profiler::AsyncFlowBeginPreflight emptyBegin = fuse::profiler::preflightBeginAsyncFlow("");
    expectTrue(emptyBegin.empty_name, "flow begin preflight marks empty name");
    expectTrue(!emptyBegin.can_begin(), "flow begin preflight rejects empty name");

    const fuse::profiler::AsyncFlowEndPreflight orphanEnd = fuse::profiler::preflightEndAsyncFlow("orphan");
    expectTrue(orphanEnd.no_open_flows, "flow end preflight marks no open flows");
    expectTrue(orphanEnd.would_orphan(), "flow end preflight would orphan without begin");
    expectTrue(!orphanEnd.can_end(), "flow end preflight rejects orphan finish");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);
    const fuse::profiler::AsyncFlowEndPreflight pairedEnd = fuse::profiler::preflightEndAsyncFlow("paired_flow");
    expectTrue(!pairedEnd.no_open_flows, "flow end preflight clears no_open_flows after begin");
    expectTrue(pairedEnd.open_flow_count == 1u, "flow end preflight reports open flow count");
    expectTrue(pairedEnd.can_end(), "flow end preflight accepts paired finish");
    FUSE_PROFILE_ASYNC_FLOW_END("paired_flow", flowId);

    fuse::profiler::setEnabled(false);
    const fuse::profiler::AsyncFlowBeginPreflight disabledBegin = fuse::profiler::preflightBeginAsyncFlow("flow");
    expectTrue(disabledBegin.profiler_disabled, "flow begin preflight marks disabled profiler");
    expectTrue(!disabledBegin.can_begin(), "flow begin preflight rejects while disabled");
}

void testCounterSamplePreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::CounterSamplePreflight nullPreflight = fuse::profiler::preflightCounterSample(nullptr);
    expectTrue(nullPreflight.null_name, "counter preflight marks null track");
    expectTrue(!nullPreflight.can_sample(), "counter preflight rejects null track");

    const fuse::profiler::CounterSamplePreflight emptyPreflight = fuse::profiler::preflightCounterSample("");
    expectTrue(emptyPreflight.empty_name, "counter preflight marks empty track");
    expectTrue(!emptyPreflight.can_sample(), "counter preflight rejects empty track");

    const fuse::profiler::CounterSamplePreflight validPreflight = fuse::profiler::preflightCounterSample("budget");
    expectTrue(validPreflight.can_sample(), "counter preflight accepts valid track while enabled");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::CounterSamplePreflight disabledPreflight = fuse::profiler::preflightCounterSample("budget");
    expectTrue(disabledPreflight.profiler_disabled, "counter preflight marks disabled profiler");
    expectTrue(!disabledPreflight.can_sample(), "counter preflight rejects while disabled");
}

void testChromeTraceExportPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.exportable(), "export preflight always exportable");
    expectTrue(emptyPreflight.buffer_empty, "export preflight marks empty buffer");
    expectTrue(!emptyPreflight.will_emit_events(), "export preflight will not emit on empty buffer");
    expectTrue(emptyPreflight.frame_index == 0u, "export preflight reports frame index");

    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("export_scope");
        FUSE_PROFILE_COUNTER("export_counter", 5);
    }

    const fuse::profiler::ChromeTraceExportPreflight populatedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!populatedPreflight.buffer_empty, "export preflight clears buffer_empty after recording");
    expectTrue(populatedPreflight.event_count == 3u, "export preflight reports event count");
    expectTrue(populatedPreflight.will_emit_events(), "export preflight will emit events");
    expectTrue(populatedPreflight.null_name_skip_count == 0u, "export preflight reports zero null-name skips");
    expectTrue(populatedPreflight.frame_index == 1u, "export preflight reflects beginFrame count");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[") != std::string::npos,
               "export preflight aligns with callable export");
}

void testEventLookupPreflightAndTryEventAt() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::EventLookupPreflight emptyPreflight = fuse::profiler::preflightEventLookup(0u);
    expectTrue(emptyPreflight.buffer_empty, "lookup preflight marks empty buffer");
    expectTrue(emptyPreflight.out_of_range, "lookup preflight marks out of range on empty buffer");
    expectTrue(emptyPreflight.should_use_sentinel(), "lookup preflight should use sentinel on empty buffer");
    expectTrue(!fuse::profiler::canLookupEventAt(0u), "canLookupEventAt false on empty buffer");

    const fuse::profiler::ProfileEvent* lookupOut = nullptr;
    expectTrue(!fuse::profiler::tryEventAt(0u, lookupOut), "tryEventAt false on empty buffer");
    expectTrue(lookupOut != nullptr && lookupOut->name == nullptr, "tryEventAt returns sentinel on empty buffer");

    {
        FUSE_PROFILE_SCOPE("lookup_scope");
    }

    const fuse::profiler::EventLookupPreflight validPreflight = fuse::profiler::preflightEventLookup(0u);
    expectTrue(!validPreflight.buffer_empty, "lookup preflight clears buffer_empty after recording");
    expectTrue(!validPreflight.out_of_range, "lookup preflight accepts in-range index");
    expectTrue(validPreflight.can_lookup(), "lookup preflight can lookup first event");
    expectTrue(fuse::profiler::canLookupEventAt(0u), "canLookupEventAt true for first event");
    expectTrue(fuse::profiler::canLookupEventAt(1u), "canLookupEventAt true for last event");
    expectTrue(!fuse::profiler::canLookupEventAt(2u), "canLookupEventAt false past event count");

    const fuse::profiler::EventLookupPreflight oobPreflight = fuse::profiler::preflightEventLookup(99u);
    expectTrue(oobPreflight.out_of_range, "lookup preflight marks out-of-range index");
    expectTrue(!oobPreflight.can_lookup(), "lookup preflight rejects out-of-range index");

    expectTrue(fuse::profiler::tryEventAt(0u, lookupOut), "tryEventAt succeeds for valid index");
    expectTrue(lookupOut != nullptr && lookupOut->name != nullptr, "tryEventAt returns valid event pointer");
    expectTrue(std::string(lookupOut->name) == "lookup_scope", "tryEventAt preserves event name");
    expectTrue(lookupOut->phase == fuse::profiler::EventPhase::Begin, "tryEventAt returns begin event");

    expectTrue(!fuse::profiler::tryEventAt(99u, lookupOut), "tryEventAt false for out-of-range index");
    expectTrue(lookupOut != nullptr && lookupOut->name == nullptr, "tryEventAt returns sentinel for out-of-range");
}

void testEmptyNameScopeGuard() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope emptyScope("");
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "empty scope name records nothing");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "empty scope name does not mutate nesting depth");
    expectTrue(!fuse::profiler::isValidProfileName(""), "isValidProfileName rejects empty string");
    expectTrue(fuse::profiler::isValidProfileName("valid"), "isValidProfileName accepts non-empty name");
    expectTrue(!fuse::profiler::isValidProfileName(nullptr), "isValidProfileName rejects null");
}

void testEmptyNameFlowAndCounterGuards() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginAsyncFlow("", 1u);
    fuse::profiler::endAsyncFlow("", 1u);
    fuse::profiler::sampleCounter("", 42);
    fuse::profiler::sampleCounterFloat("", 1.5);
    fuse::profiler::sampleCounterSnapshotAtFrame("", 7);
    fuse::profiler::sampleCounterFloatSnapshotAtFrame("", 0.25);

    expectTrue(fuse::profiler::eventCount() == 0u, "empty flow/counter names record nothing");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "empty flow names do not mutate flow depth");
    expectTrue(fuse::profiler::preflightBeginAsyncFlow("").emptyName,
               "begin preflight marks empty name");
    expectTrue(!fuse::profiler::preflightBeginAsyncFlow("").canBegin(),
               "begin preflight rejects empty name");
    expectTrue(fuse::profiler::preflightEndAsyncFlow("").emptyName,
               "end preflight marks empty name");
    expectTrue(!fuse::profiler::preflightEndAsyncFlow("").canEnd(),
               "end preflight rejects empty name");
}

void testTryEventAtLookupStub() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent out{};
    expectTrue(!fuse::profiler::tryEventAt(0u, out), "tryEventAt false on empty buffer");
    expectTrue(out.name == nullptr, "tryEventAt leaves out sentinel on failure");

    {
        FUSE_PROFILE_SCOPE("lookup_scope");
    }

    expectTrue(fuse::profiler::tryEventAt(0u, out), "tryEventAt true for first event");
    expectTrue(out.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin phase");
    expectTrue(std::string(out.name) == "lookup_scope", "tryEventAt copies event name");
    expectTrue(!fuse::profiler::tryEventAt(99u, out), "tryEventAt false when out of range");
}

void testHasLastEventLookupStub() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::hasLastEvent(), "hasLastEvent false on empty buffer");

    {
        FUSE_PROFILE_SCOPE("last_lookup_scope");
    }

    expectTrue(fuse::profiler::hasLastEvent(), "hasLastEvent true after recording");
    expectTrue(fuse::profiler::hasLastEvent() == fuse::profiler::isValidProfileEvent(fuse::profiler::lastEvent()),
               "hasLastEvent mirrors lastEvent validity");
}

void testAsyncFlowPreflights() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::AsyncFlowEndPreflight orphanPreflight = fuse::profiler::preflightEndAsyncFlow("flow");
    expectTrue(orphanPreflight.orphanEnd, "end preflight marks orphan finish");
    expectTrue(!orphanPreflight.canEnd(), "end preflight rejects orphan finish");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    const fuse::profiler::AsyncFlowBeginPreflight beginPreflight =
        fuse::profiler::preflightBeginAsyncFlow("paired_flow");
    expectTrue(beginPreflight.canBegin(), "begin preflight accepts valid name");
    expectTrue(!beginPreflight.profilerDisabled, "begin preflight clears profilerDisabled");
    expectTrue(!beginPreflight.emptyName, "begin preflight clears emptyName");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);
    const fuse::profiler::AsyncFlowEndPreflight matchedPreflight =
        fuse::profiler::preflightEndAsyncFlow("paired_flow");
    expectTrue(matchedPreflight.canEnd(), "end preflight accepts matched begin");
    expectTrue(!matchedPreflight.orphanEnd, "matched end preflight clears orphanEnd");
    FUSE_PROFILE_ASYNC_FLOW_END("paired_flow", flowId);

    fuse::profiler::setEnabled(false);
    const fuse::profiler::AsyncFlowBeginPreflight disabledBegin =
        fuse::profiler::preflightBeginAsyncFlow("ignored");
    expectTrue(disabledBegin.profilerDisabled, "disabled profiler marks begin preflight");
    expectTrue(!disabledBegin.canBegin(), "disabled profiler rejects begin preflight");
}

void testNestingStatePreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::NestingStatePreflight initial = fuse::profiler::preflightNestingState();
    expectTrue(initial.isBalanced(), "reset nesting preflight is balanced");
    expectTrue(initial.scopeDepth == 0u, "reset nesting preflight scope depth zero");
    expectTrue(initial.flowDepth == 0u, "reset nesting preflight flow depth zero");
    expectTrue(initial.openAsyncFlows == 0u, "reset nesting preflight open flows zero");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("nest_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("nest_flow", flowId);
        const fuse::profiler::NestingStatePreflight nested = fuse::profiler::preflightNestingState();
        expectTrue(nested.scopeDepth == 1u, "nested preflight reports scope depth");
        expectTrue(nested.flowDepth == 1u, "nested preflight reports flow depth");
        expectTrue(nested.openAsyncFlows == 1u, "nested preflight reports open async flows");
        expectTrue(nested.hasUnbalancedAsyncFlows, "open flow marks unbalanced async flows");
        expectTrue(!nested.isBalanced(), "open flow nesting preflight is unbalanced");
        FUSE_PROFILE_ASYNC_FLOW_END("nest_flow", flowId);
    }

    const fuse::profiler::NestingStatePreflight finalState = fuse::profiler::preflightNestingState();
    expectTrue(finalState.isBalanced(), "paired flow restores balanced nesting preflight");
    expectTrue(finalState.maxScopeDepth >= 1u, "nesting preflight reports max scope depth");
    expectTrue(finalState.maxFlowDepth >= 1u, "nesting preflight reports max flow depth");
}

void testChromeExportPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeExportPreflight emptyPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(emptyPreflight.canExport(), "empty buffer can export");
    expectTrue(emptyPreflight.bufferEmpty, "empty preflight marks buffer empty");
    expectTrue(emptyPreflight.eventCount == 0u, "empty preflight event count zero");
    expectTrue(emptyPreflight.exportableEventCount == 0u, "empty preflight exportable count zero");
    expectTrue(emptyPreflight.droppedEventCount == 0u, "empty preflight dropped count zero");
    expectTrue(fuse::profiler::canExportChromeTrace(), "canExportChromeTrace true when enabled");

    {
        FUSE_PROFILE_SCOPE("export_scope");
        FUSE_PROFILE_COUNTER("export_counter", 5);
    }

    const fuse::profiler::ChromeExportPreflight filledPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(filledPreflight.canExport(), "filled buffer can export");
    expectTrue(!filledPreflight.bufferEmpty, "filled preflight clears bufferEmpty");
    expectTrue(filledPreflight.eventCount == 3u, "filled preflight reports event count");
    expectTrue(filledPreflight.exportableEventCount == 3u, "filled preflight counts exportable events");
    expectTrue(filledPreflight.skippedInvalidNameCount == 0u, "filled preflight skips no valid names");

    fuse::profiler::setEnabled(false);
    const fuse::profiler::ChromeExportPreflight disabledPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(disabledPreflight.profilerDisabled, "disabled profiler marks export preflight");
    expectTrue(!disabledPreflight.canExport(), "disabled profiler rejects export preflight");
    expectTrue(!fuse::profiler::canExportChromeTrace(), "canExportChromeTrace false when disabled");
}

void testDroppedEventCountTracking() {
    resetState();
    fuse::platform::registerMainThread();

    for (fuse::u32 i = 0; i < 4097u; ++i) {
        fuse::profiler::sampleCounter("overflow_track", static_cast<fuse::s64>(i));
    }

    expectTrue(fuse::profiler::isBufferFull(), "overflow recording saturates ring buffer");
    expectTrue(fuse::profiler::eventCount() == 4096u, "event count caps at ring capacity");
    expectTrue(fuse::profiler::droppedEventCount() == 1u, "overflow increments dropped event count");

    const fuse::profiler::ChromeExportPreflight overflowPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(overflowPreflight.droppedEventCount == 1u, "export preflight reports dropped events");

    fuse::profiler::reset();
    expectTrue(fuse::profiler::droppedEventCount() == 0u, "reset clears dropped event count");
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
    expectTrue(!fuse::profiler::isValidEventName(nullptr), "null name is invalid");
    expectTrue(!fuse::profiler::isValidEventName(""), "empty-string name is invalid");
    expectTrue(fuse::profiler::isValidEventName("valid"), "non-empty name is valid");
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
    expectTrue(fuse::profiler::isValidEventName("valid"), "non-empty name is valid");
}

void testEmptyProfileEventSentinel() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ProfileEvent& sentinel = fuse::profiler::emptyProfileEvent();
    expectTrue(sentinel.name == nullptr, "emptyProfileEvent has null name");
    expectTrue(!fuse::profiler::isValidProfileEvent(sentinel),
               "emptyProfileEvent is not a valid profile event");

    const fuse::profiler::ProfileEvent& emptyLookup = fuse::profiler::eventAt(0);
    expectTrue(&emptyLookup == &sentinel, "eventAt on empty buffer returns emptyProfileEvent sentinel");

    const fuse::profiler::ProfileEvent& oobLookup = fuse::profiler::eventAt(99);
    expectTrue(&oobLookup == &sentinel, "eventAt out-of-range returns emptyProfileEvent sentinel");
}

void testTryLastEventGuard() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent outEvent{};
    expectTrue(!fuse::profiler::tryLastEvent(outEvent), "tryLastEvent false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryLastEvent clears output on empty buffer");

    {
        FUSE_PROFILE_SCOPE("last_scope");
    }

    expectTrue(fuse::profiler::tryLastEvent(outEvent), "tryLastEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastEvent copies last end phase");
    expectTrue(outEvent.name != nullptr && std::string(outEvent.name) == "last_scope",
               "tryLastEvent copies last scope name");
    expectTrue(&fuse::profiler::lastEvent() == &fuse::profiler::eventAt(fuse::profiler::lastEventIndex()),
               "lastEvent matches eventAt(lastEventIndex)");
}

void testRingCapacityIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::ringCapacity() == 4096u, "ring capacity reports 4096 events");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringCapacity(),
               "event count stays below ring capacity after reset");
}

void testChromeExportPreflightEmptyBuffer() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::canExportChromeTrace(), "canExportChromeTrace false on empty buffer");

    const fuse::profiler::ChromeExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!preflight.canExport(), "preflight canExport false on empty buffer");
    expectTrue(preflight.bufferEmpty, "preflight reports empty buffer");
    expectTrue(!preflight.profilerDisabled, "preflight reports profiler enabled after reset");
    expectTrue(!preflight.unbalancedScopeNesting, "preflight reports balanced scope nesting after reset");
    expectTrue(!preflight.openAsyncFlows, "preflight reports no open async flows after reset");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"traceEvents\":[]") != std::string::npos,
               "export still emits valid empty trace on preflight failure");
}

void testChromeExportPreflightWithEvents() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("export_preflight_scope");
    }

    expectTrue(fuse::profiler::canExportChromeTrace(), "canExportChromeTrace true with events");

    const fuse::profiler::ChromeExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.canExport(), "preflight canExport true with events");
    expectTrue(!preflight.bufferEmpty, "preflight reports non-empty buffer");
    expectTrue(!preflight.unbalancedScopeNesting, "preflight reports balanced scope nesting after scope end");
    expectTrue(!preflight.openAsyncFlows, "preflight reports no open async flows");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"export_preflight_scope\"") != std::string::npos,
               "export after successful preflight includes scope name");
}

void testChromeExportPreflightUnbalancedNesting() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("unbalanced_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("unbalanced_flow", flowId);
    }

    const fuse::profiler::ChromeExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.canExport(), "preflight canExport true despite open flow");
    expectTrue(!preflight.bufferEmpty, "preflight reports events recorded");
    expectTrue(preflight.unbalancedScopeNesting == false, "ended scope leaves nesting balanced");
    expectTrue(preflight.openAsyncFlows, "preflight reports open async flow after unmatched begin");

    fuse::profiler::reset();
}

void testChromeExportPreflightDisabledProfiler() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("before_disable");
    }
    fuse::profiler::setEnabled(false);

    const fuse::profiler::ChromeExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.canExport(), "preflight canExport true for buffered events while disabled");
    expectTrue(preflight.profilerDisabled, "preflight reports profiler disabled");
    expectTrue(!preflight.bufferEmpty, "preflight still sees buffered events while disabled");
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

void testChromeExportPreflight() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::profiler::ChromeExportPreflight emptyPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(emptyPreflight.canExport(), "empty buffer can still export");
    expectTrue(emptyPreflight.bufferEmpty, "preflight marks empty buffer");
    expectTrue(!emptyPreflight.hasStateWarnings(), "clean reset has no state warnings");
    expectTrue(!emptyPreflight.unbalancedScopeNesting, "reset leaves scope nesting balanced");
    expectTrue(!emptyPreflight.unbalancedFlowNesting, "reset leaves flow nesting balanced");
    expectTrue(!emptyPreflight.hasOpenAsyncFlows, "reset leaves no open async flows");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("export_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("export_flow", flowId);
    }

    const fuse::profiler::ChromeExportPreflight dirtyPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(dirtyPreflight.canExport(), "unbalanced state can still export");
    expectTrue(!dirtyPreflight.bufferEmpty, "preflight sees recorded events");
    expectTrue(dirtyPreflight.hasStateWarnings(), "unmatched flow triggers state warnings");
    expectTrue(!dirtyPreflight.unbalancedScopeNesting, "ended scope is balanced");
    expectTrue(dirtyPreflight.unbalancedFlowNesting, "unmatched flow marks flow nesting unbalanced");
    expectTrue(dirtyPreflight.hasOpenAsyncFlows, "unmatched flow marks open async flows");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"traceEvents\":[") != std::string::npos,
               "preflight does not block chrome export");
    expectTrue(json.find("\"name\":\"export_outer\"") != std::string::npos,
               "preflight export includes recorded scope");
}

void testEmptyStringNameGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::isValidEventName(nullptr), "null name is invalid");
    expectTrue(!fuse::profiler::isValidEventName(""), "empty string name is invalid");
    expectTrue(fuse::profiler::isValidEventName("valid"), "non-empty name is valid");

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
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "empty scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "empty flow name does not mutate flow depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "empty flow name does not increment open count");
    expectTrue(fuse::profiler::isExportEmpty(), "empty-string guard leaves export empty");
    expectTrue(fuse::profiler::exportableEventCount() == 0u, "exportable count is zero with no valid events");
}

void testHasOpenScopesAndAsyncFlowsIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::hasOpenScopes(), "reset leaves hasOpenScopes false");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "reset leaves hasOpenAsyncFlows false");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("introspect_scope");
        expectTrue(fuse::profiler::hasOpenScopes(), "hasOpenScopes true inside scope");
        expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false before flow begin");

        FUSE_PROFILE_ASYNC_FLOW_BEGIN("introspect_flow", flowId);
        expectTrue(fuse::profiler::hasOpenScopes(), "hasOpenScopes true with nested flow");
        expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true after flow begin");

        FUSE_PROFILE_ASYNC_FLOW_END("introspect_flow", flowId);
        expectTrue(fuse::profiler::hasOpenScopes(), "hasOpenScopes still true after flow end");
        expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after flow end");
    }
    expectTrue(!fuse::profiler::hasOpenScopes(), "hasOpenScopes false after scope end");
}

void testExportPreflightStubs() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::canExportChromeTrace(), "canExportChromeTrace true on empty buffer");
    expectTrue(fuse::profiler::isExportEmpty(), "isExportEmpty true on reset");
    expectTrue(fuse::profiler::exportableEventCount() == 0u, "exportableEventCount zero on reset");

    {
        FUSE_PROFILE_SCOPE("preflight_scope");
    }

    expectTrue(fuse::profiler::canExportChromeTrace(), "canExportChromeTrace true with events");
    expectTrue(!fuse::profiler::isExportEmpty(), "isExportEmpty false after recording");
    expectTrue(fuse::profiler::exportableEventCount() == 2u, "exportableEventCount matches valid scope events");
    expectTrue(fuse::profiler::exportableEventCount() == fuse::profiler::eventCount(),
               "exportable count matches event count for valid names");

    fuse::profiler::beginAsyncFlow("", 99u);
    expectTrue(fuse::profiler::exportableEventCount() == fuse::profiler::eventCount(),
               "empty-name guard does not inflate exportable count");
}

void testTryEventAtSafeLookup() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent out{};
    expectTrue(!fuse::profiler::tryEventAt(0u, out), "tryEventAt false on empty buffer");
    expectTrue(!fuse::profiler::isValidProfileEvent(out), "tryEventAt out param unchanged on failure");

    {
        FUSE_PROFILE_SCOPE("lookup_scope");
    }

    expectTrue(fuse::profiler::tryEventAt(0u, out), "tryEventAt true for first event");
    expectTrue(out.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin phase");
    expectTrue(std::string(out.name) == "lookup_scope", "tryEventAt copies event name");

    expectTrue(fuse::profiler::tryEventAt(1u, out), "tryEventAt true for second event");
    expectTrue(out.phase == fuse::profiler::EventPhase::End, "tryEventAt copies end phase");

    expectTrue(!fuse::profiler::tryEventAt(2u, out), "tryEventAt false past event count");
    expectTrue(!fuse::profiler::tryEventAt(99u, out), "tryEventAt false for out-of-range index");
}

void testIsValidProfileEventRejectsEmptyName() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::ProfileEvent emptyNameEvent{};
    emptyNameEvent.name = "";
    expectTrue(!fuse::profiler::isValidProfileEvent(emptyNameEvent),
               "isValidProfileEvent rejects empty string name");

    fuse::profiler::ProfileEvent nullNameEvent{};
    expectTrue(!fuse::profiler::isValidProfileEvent(nullNameEvent),
               "isValidProfileEvent rejects null name");

    {
        FUSE_PROFILE_SCOPE("valid_event");
    }
    expectTrue(fuse::profiler::isValidProfileEvent(fuse::profiler::eventAt(0)),
               "isValidProfileEvent accepts recorded event");
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
    testDisabledProfilerEndBalancesActiveFlowDepth();
    testDisabledProfilerSkipsSnapshotCounter();
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
    testScopeNestingDepthIntrospection();
    testRingCapacityAndBufferFullGuards();
    testOpenAsyncFlowCountIntrospection();
    testNullNameScopeGuard();
    testDisabledAsyncFlowBeginDoesNotOpenFlow();
    testResetClearsOpenAsyncFlowState();
    testOrphanAsyncFlowEndIsIgnored();
    testNullNameFlowAndCounterGuards();
    testNullScopeNameGuard();
    testDisabledProfilerDoesNotMutateScopeNestingDepth();
    testDisabledAsyncFlowBeginSkipsDepthAndOpenCount();
    testResetClearsOpenAsyncFlowCount();
    testDisabledCounterPreservesFlowNestingDepth();
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
    testEmptyProfileEventSentinel();
    testRingCapacityIntrospection();
    testChromeExportPreflightEmptyBuffer();
    testChromeExportPreflightWithEvents();
    testChromeExportPreflightUnbalancedNesting();
    testChromeExportPreflightDisabledProfiler();
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
    testOpenAsyncFlowCountGuards();
    testNullNameScopeGuard();
    testDoubleOrphanAsyncFlowEndIsIgnored();
    testDisabledScopePreservesNestingDepth();
    testDisabledScopeDoesNotMutateScopeNestingDepth();
    testDisabledAsyncFlowEndPreservesOpenCount();
    testCrossThreadFlowFinishSkipsWorkerFlowDepthPop();
    testResetClearsNestingAndOpenFlowState();
    testDisabledScopeLiveNestingGuard();
    testHasOpenAsyncFlowsIntrospection();
    testRingBufferCapacityGuard();
    testNestingDepthAliasesMatch();
    testRingCapacityAndHasOpenAsyncFlowsGuards();
    testEmptyProfileEventSentinel();
    testDisabledProfilerSkipsSnapshotCounter();
    testIsNonEmptyProfileNameGuard();
    testEmptyNameScopeFlowAndCounterGuards();
    testTryEventAtAndTryLastEvent();
    testPreflightChromeTraceExport();
    testDisabledRecordPreflight();
    testRingCapacityAndDroppedEventCount();
    testHasOpenAsyncFlowsGuard();
    testIsValidProfileNamePreflight();
    testNestingBalancePreflights();
    testChromeTraceExportPreflight();
    testSafeLookupStubs();
    testRingCapacityIntrospection();
    testChromeExportPreflight();
    testEmptyNamePreflightGuards();
    testScopeNestingPreflight();
    testAsyncFlowPreflights();
    testSafeEventLookupStubs();
    testRingBufferCapacityIntrospection();
    testProfileNamePreflight();
    testScopePreflight();
    testCounterSamplePreflight();
    testEventLookupPreflightAndTryEventAt();
    testIsValidProfileNameGuard();
    testTryEventAtSafeLookup();
    testPreflightGuardState();
    testEmptyNameScopeGuard();
    testEmptyNameFlowAndCounterGuards();
    testTryEventAtLookupStub();
    testHasLastEventLookupStub();
    testNestingStatePreflight();
    testDroppedEventCountTracking();
    testHasOpenScopesAndAsyncFlowsIntrospection();
    testExportPreflightStubs();
    testIsValidProfileEventRejectsEmptyName();
    testFatalHandlerHook();
    testVerifyMacro();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_profiler_assert: all tests passed\n");
    return EXIT_SUCCESS;
}
