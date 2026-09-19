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

// --- deepen additive from deepen-b16-profiler-guards-77ef ---
void testRingCapacityAndBufferFullGuards() {
void testNullNameScopeGuard() {

// --- deepen additive from deepen-b16-profiler-guards-2e20 ---
void testOpenAsyncFlowCountGuards() {

// --- deepen additive from deepen-b16-profiler-c977 ---
void testIsNonEmptyProfileNameGuard() {
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
    expectTrue(fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt succeeds for first event");
    expectTrue(std::string(outEvent.name) == "try_scope", "tryEventAt copies event name");
    expectTrue(!fuse::profiler::tryEventAt(99u, outEvent), "tryEventAt false for out-of-range index");
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
    const fuse::profiler::ProfilerExportPreflight recordedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(recordedPreflight.has_events, "recorded export preflight has events");
    expectTrue(recordedPreflight.can_export(), "recorded export preflight can export");
    expectTrue(recordedPreflight.event_count == 2u, "recorded export preflight reports event count");
void testDisabledRecordPreflight() {
    const fuse::profiler::ProfilerRecordPreflight disabledPreflight = fuse::profiler::preflightRecord("scope");
    expectTrue(!disabledPreflight.profiler_enabled, "disabled profiler preflight reports disabled");
    expectTrue(disabledPreflight.name_valid, "disabled profiler preflight still validates name");
    expectTrue(!disabledPreflight.can_record_scope(), "disabled profiler preflight blocks scope");
    expectTrue(!disabledPreflight.can_record_counter(), "disabled profiler preflight blocks counter");
    const fuse::profiler::ProfilerExportPreflight overflowPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(overflowPreflight.buffer_full, "overflow preflight reports full buffer");
    expectTrue(overflowPreflight.dropped_event_count == 1u, "overflow preflight reports dropped count");
    expectTrue(overflowPreflight.can_export(), "full buffer still has exportable events");
void testHasOpenAsyncFlowsGuard() {
    testPreflightChromeTraceExport();
    testDisabledRecordPreflight();

// --- deepen additive from deepen-b16-profiler-preflights-ea42 ---
void testIsValidProfileNamePreflight() {
void testNestingBalancePreflights() {
void testChromeTraceExportPreflight() {
    const fuse::profiler::ChromeTraceExportPreflight empty = fuse::profiler::preflightChromeTraceExport();
    const fuse::profiler::ChromeTraceExportPreflight recorded = fuse::profiler::preflightChromeTraceExport();
    const fuse::profiler::ChromeTraceExportPreflight openFlow = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!fuse::profiler::tryEventAt(0, out), "tryEventAt fails on empty buffer");
    expectTrue(fuse::profiler::tryEventAt(0, out), "tryEventAt succeeds for valid index");
    expectTrue(out.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin event");
    expectTrue(std::string(out.name) == "lookup_scope", "tryEventAt preserves event name");
    expectTrue(!fuse::profiler::tryEventAt(99, out), "tryEventAt fails for out-of-range index");
    testIsValidProfileNamePreflight();
    testNestingBalancePreflights();
    testChromeTraceExportPreflight();

// --- deepen additive from deepen-b16-profiler-guards-0c1a ---
void testRingCapacityAndHasOpenAsyncFlowsGuards() {
void testChromeExportPreflight() {
    const fuse::profiler::ChromeExportPreflight emptyPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(emptyPreflight.canExport, "empty preflight allows export");
    expectTrue(emptyPreflight.bufferEmpty, "empty preflight reports empty buffer");
    expectTrue(!emptyPreflight.hasEvents, "empty preflight reports no events");
    expectTrue(emptyPreflight.eventCount == 0u, "empty preflight reports zero event count");
    expectTrue(emptyPreflight.scopeNestingBalanced, "empty preflight reports balanced scope nesting");
    expectTrue(emptyPreflight.flowNestingBalanced, "empty preflight reports balanced flow nesting");
    expectTrue(!emptyPreflight.hasOpenAsyncFlows, "empty preflight reports no open async flows");
    const fuse::profiler::ChromeExportPreflight activePreflight = fuse::profiler::preflightChromeExport();
    expectTrue(activePreflight.canExport, "active preflight allows export");
    expectTrue(!activePreflight.bufferEmpty, "active preflight reports non-empty buffer");
    expectTrue(activePreflight.hasEvents, "active preflight reports events");
    expectTrue(activePreflight.eventCount >= 4u, "active preflight reports recorded event count");
    expectTrue(activePreflight.scopeNestingBalanced, "ended scope leaves nesting balanced in preflight");
    expectTrue(!activePreflight.flowNestingBalanced, "unmatched flow leaves flow nesting unbalanced in preflight");
    expectTrue(activePreflight.hasOpenAsyncFlows, "unmatched flow reports open async flows in preflight");
    testChromeExportPreflight();

// --- deepen additive from profiler-b16-preflight-deepen-6ec7 ---
void testEmptyNamePreflightGuards() {
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
void testScopeNestingPreflight() {
    const fuse::profiler::ScopeNestingPreflight initial = fuse::profiler::preflightScopeNesting();
        const fuse::profiler::ScopeNestingPreflight nested = fuse::profiler::preflightScopeNesting();
    const fuse::profiler::ScopeNestingPreflight disabled = fuse::profiler::preflightScopeNesting();
void testAsyncFlowPreflights() {
    const fuse::profiler::AsyncFlowEndPreflight orphanPreflight = fuse::profiler::preflightAsyncFlowEnd("flow");
    expectTrue(orphanPreflight.orphan_end, "preflight flags orphan end with no open flows");
    expectTrue(!orphanPreflight.canEnd(), "orphan end cannot proceed");
    expectTrue(orphanPreflight.shouldSkip(), "orphan end should skip");
    const fuse::profiler::AsyncFlowBeginPreflight beginPreflight =
        fuse::profiler::preflightAsyncFlowBegin("vfs_load");
    expectTrue(beginPreflight.canBegin(), "valid begin preflight passes");
    expectTrue(!beginPreflight.shouldSkip(), "valid begin should not skip");
    const fuse::profiler::AsyncFlowEndPreflight matchedPreflight = fuse::profiler::preflightAsyncFlowEnd("vfs_load");
    expectTrue(!matchedPreflight.orphan_end, "preflight clears orphan flag after begin");
    expectTrue(matchedPreflight.canEnd(), "matched end preflight passes");
    const fuse::profiler::AsyncFlowBeginPreflight nullBegin = fuse::profiler::preflightAsyncFlowBegin(nullptr);
    const fuse::profiler::ChromeTraceExportPreflight emptyPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(emptyPreflight.would_emit_empty_trace, "empty buffer would emit empty trace");
    expectTrue(!emptyPreflight.has_events, "empty buffer has no events");
    expectTrue(emptyPreflight.shouldSkip(), "empty export should skip");
    expectTrue(emptyPreflight.canExport(), "export preflight always allows export");
    const fuse::profiler::ChromeTraceExportPreflight withEvents = fuse::profiler::preflightChromeTraceExport();
    const fuse::profiler::ChromeTraceExportPreflight unmatched = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!fuse::profiler::tryEventAt(0u, out), "tryEventAt false on empty buffer");
    expectTrue(fuse::profiler::tryEventAt(0u, out), "tryEventAt true for first event");
               "tryEventAt copies scope begin name");
    expectTrue(out.phase == fuse::profiler::EventPhase::Begin, "tryEventAt copies begin phase");
    expectTrue(fuse::profiler::tryEventAt(1u, out), "tryEventAt true for second event");
    expectTrue(out.phase == fuse::profiler::EventPhase::End, "tryEventAt copies end phase");
    expectTrue(!fuse::profiler::tryEventAt(2u, out), "tryEventAt false past event count");
    testEmptyNamePreflightGuards();
    testScopeNestingPreflight();
    testAsyncFlowPreflights();

// --- deepen additive from deepen-b16-profiler-guards-a7f2 ---
    expectTrue(emptyPreflight.canExport(), "preflight allows export when profiler is enabled");
    expectTrue(emptyPreflight.profilerEnabled, "preflight reports profiler enabled");
    expectTrue(emptyPreflight.bufferEmpty, "preflight reports empty buffer on reset");
    expectTrue(!emptyPreflight.hasExportableEvents, "preflight reports no exportable events on reset");
    expectTrue(emptyPreflight.exportableEventCount == 0u, "preflight exportable count is zero on reset");
    expectTrue(emptyPreflight.scopeNestingBalanced, "preflight reports balanced scope nesting on reset");
    expectTrue(emptyPreflight.flowNestingBalanced, "preflight reports balanced flow nesting on reset");
    expectTrue(!emptyPreflight.hasOpenAsyncFlows, "preflight reports no open flows on reset");
    expectTrue(activePreflight.canExport(), "preflight allows export with recorded events");
    expectTrue(!activePreflight.bufferEmpty, "preflight reports non-empty buffer");
    expectTrue(activePreflight.hasExportableEvents, "preflight reports exportable events");
    expectTrue(activePreflight.exportableEventCount == activePreflight.eventCount,
    expectTrue(activePreflight.frameIndex == 1u, "preflight reports current frame index");
    expectTrue(activePreflight.hasOpenAsyncFlows, "preflight reports open async flows");
    expectTrue(!disabledPreflight.canExport(), "preflight blocks export when profiler is disabled");
    expectTrue(!disabledPreflight.profilerEnabled, "preflight reports profiler disabled");

// --- deepen additive from deepen-b16-profiler-preflights-4b82 ---
void testProfileNamePreflight() {
    expectTrue(nullPreflight.null_name, "null name preflight marks null_name");
    expectTrue(!nullPreflight.empty_name, "null name preflight clears empty_name");
    expectTrue(!nullPreflight.can_record(), "null name preflight cannot record");
    expectTrue(nullPreflight.should_skip(), "null name preflight should skip");
    expectTrue(!emptyPreflight.null_name, "empty string preflight clears null_name");
    expectTrue(emptyPreflight.empty_name, "empty string preflight marks empty_name");
    expectTrue(!emptyPreflight.can_record(), "empty string preflight cannot record");
    const fuse::profiler::ProfileNamePreflight validPreflight = fuse::profiler::preflightProfileName("valid");
    expectTrue(!validPreflight.null_name, "valid name preflight clears null_name");
    expectTrue(!validPreflight.empty_name, "valid name preflight clears empty_name");
    expectTrue(validPreflight.can_record(), "valid name preflight can record");
    expectTrue(!validPreflight.should_skip(), "valid name preflight should not skip");
void testScopePreflight() {
    const fuse::profiler::ScopePreflight nullPreflight = fuse::profiler::preflightScope(nullptr);
    expectTrue(nullPreflight.null_name, "scope preflight marks null name");
    expectTrue(!nullPreflight.can_enter(), "scope preflight rejects null name");
    const fuse::profiler::ScopePreflight emptyPreflight = fuse::profiler::preflightScope("");
    expectTrue(emptyPreflight.empty_name, "scope preflight marks empty name");
    expectTrue(!emptyPreflight.can_enter(), "scope preflight rejects empty name");
    const fuse::profiler::ScopePreflight validPreflight = fuse::profiler::preflightScope("valid_scope");
    expectTrue(validPreflight.can_enter(), "scope preflight accepts valid name while enabled");
    const fuse::profiler::ScopePreflight disabledPreflight = fuse::profiler::preflightScope("valid_scope");
    expectTrue(disabledPreflight.profiler_disabled, "scope preflight marks disabled profiler");
    expectTrue(!disabledPreflight.can_enter(), "scope preflight rejects while disabled");
    const fuse::profiler::ScopeNestingPreflight rootPreflight = fuse::profiler::preflightScopeNesting();
    expectTrue(rootPreflight.is_at_root(), "nesting preflight at root after reset");
    expectTrue(rootPreflight.current_depth == 0u, "nesting preflight reports zero current depth");
    expectTrue(rootPreflight.max_observed_depth == 0u, "nesting preflight reports zero max depth");
        const fuse::profiler::ScopeNestingPreflight outerPreflight = fuse::profiler::preflightScopeNesting();
        expectTrue(!outerPreflight.is_at_root(), "nesting preflight not at root inside scope");
        expectTrue(outerPreflight.current_depth == 1u, "nesting preflight reports depth 1 in outer scope");
            const fuse::profiler::ScopeNestingPreflight innerPreflight = fuse::profiler::preflightScopeNesting();
            expectTrue(innerPreflight.current_depth == 2u, "nesting preflight reports depth 2 in inner scope");
            expectTrue(innerPreflight.max_observed_depth == 2u, "nesting preflight tracks max observed depth");
    const fuse::profiler::ScopeNestingPreflight afterPreflight = fuse::profiler::preflightScopeNesting();
    expectTrue(afterPreflight.is_at_root(), "nesting preflight returns to root after scopes end");
    const fuse::profiler::AsyncFlowBeginPreflight nullBegin = fuse::profiler::preflightBeginAsyncFlow(nullptr);
    const fuse::profiler::AsyncFlowBeginPreflight emptyBegin = fuse::profiler::preflightBeginAsyncFlow("");
    const fuse::profiler::AsyncFlowEndPreflight orphanEnd = fuse::profiler::preflightEndAsyncFlow("orphan");
    const fuse::profiler::AsyncFlowEndPreflight pairedEnd = fuse::profiler::preflightEndAsyncFlow("paired_flow");
    const fuse::profiler::AsyncFlowBeginPreflight disabledBegin = fuse::profiler::preflightBeginAsyncFlow("flow");
void testCounterSamplePreflight() {
    const fuse::profiler::CounterSamplePreflight nullPreflight = fuse::profiler::preflightCounterSample(nullptr);
    expectTrue(nullPreflight.null_name, "counter preflight marks null track");
    expectTrue(!nullPreflight.can_sample(), "counter preflight rejects null track");
    const fuse::profiler::CounterSamplePreflight emptyPreflight = fuse::profiler::preflightCounterSample("");
    expectTrue(emptyPreflight.empty_name, "counter preflight marks empty track");
    expectTrue(!emptyPreflight.can_sample(), "counter preflight rejects empty track");
    const fuse::profiler::CounterSamplePreflight validPreflight = fuse::profiler::preflightCounterSample("budget");
    expectTrue(validPreflight.can_sample(), "counter preflight accepts valid track while enabled");
    const fuse::profiler::CounterSamplePreflight disabledPreflight = fuse::profiler::preflightCounterSample("budget");
    expectTrue(disabledPreflight.profiler_disabled, "counter preflight marks disabled profiler");
    expectTrue(!disabledPreflight.can_sample(), "counter preflight rejects while disabled");
    expectTrue(emptyPreflight.exportable(), "export preflight always exportable");
    expectTrue(emptyPreflight.buffer_empty, "export preflight marks empty buffer");
    expectTrue(!emptyPreflight.will_emit_events(), "export preflight will not emit on empty buffer");
    expectTrue(emptyPreflight.frame_index == 0u, "export preflight reports frame index");
    const fuse::profiler::ChromeTraceExportPreflight populatedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!populatedPreflight.buffer_empty, "export preflight clears buffer_empty after recording");
    expectTrue(populatedPreflight.event_count == 3u, "export preflight reports event count");
    expectTrue(populatedPreflight.will_emit_events(), "export preflight will emit events");
    expectTrue(populatedPreflight.null_name_skip_count == 0u, "export preflight reports zero null-name skips");
    expectTrue(populatedPreflight.frame_index == 1u, "export preflight reflects beginFrame count");
void testEventLookupPreflightAndTryEventAt() {
    const fuse::profiler::EventLookupPreflight emptyPreflight = fuse::profiler::preflightEventLookup(0u);
    expectTrue(emptyPreflight.buffer_empty, "lookup preflight marks empty buffer");
    expectTrue(emptyPreflight.out_of_range, "lookup preflight marks out of range on empty buffer");
    expectTrue(emptyPreflight.should_use_sentinel(), "lookup preflight should use sentinel on empty buffer");
    expectTrue(!fuse::profiler::tryEventAt(0u, lookupOut), "tryEventAt false on empty buffer");
    expectTrue(lookupOut != nullptr && lookupOut->name == nullptr, "tryEventAt returns sentinel on empty buffer");
    const fuse::profiler::EventLookupPreflight validPreflight = fuse::profiler::preflightEventLookup(0u);
    expectTrue(!validPreflight.buffer_empty, "lookup preflight clears buffer_empty after recording");
    expectTrue(!validPreflight.out_of_range, "lookup preflight accepts in-range index");
    expectTrue(validPreflight.can_lookup(), "lookup preflight can lookup first event");
    const fuse::profiler::EventLookupPreflight oobPreflight = fuse::profiler::preflightEventLookup(99u);
    expectTrue(oobPreflight.out_of_range, "lookup preflight marks out-of-range index");
    expectTrue(!oobPreflight.can_lookup(), "lookup preflight rejects out-of-range index");
    expectTrue(fuse::profiler::tryEventAt(0u, lookupOut), "tryEventAt succeeds for valid index");
    expectTrue(lookupOut != nullptr && lookupOut->name != nullptr, "tryEventAt returns valid event pointer");
    expectTrue(std::string(lookupOut->name) == "lookup_scope", "tryEventAt preserves event name");
    expectTrue(lookupOut->phase == fuse::profiler::EventPhase::Begin, "tryEventAt returns begin event");
    expectTrue(!fuse::profiler::tryEventAt(99u, lookupOut), "tryEventAt false for out-of-range index");
    expectTrue(lookupOut != nullptr && lookupOut->name == nullptr, "tryEventAt returns sentinel for out-of-range");
    testProfileNamePreflight();
    testScopePreflight();
    testCounterSamplePreflight();
    testEventLookupPreflightAndTryEventAt();

// --- deepen additive from deepen-b16-profiler-preflights-8f4e ---
void testIsValidProfileNameGuard() {
    expectTrue(!fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt fails on empty buffer");
    expectTrue(outEvent != nullptr, "tryEventAt sets out pointer on failure");
    expectTrue(outEvent->name == nullptr, "tryEventAt failure returns empty sentinel");
               "tryEventAt sentinel is not a valid profile event");
    expectTrue(outEvent != nullptr && outEvent->name != nullptr, "tryEventAt returns recorded event");
    expectTrue(outEvent->phase == fuse::profiler::EventPhase::Begin, "tryEventAt preserves event phase");
    expectTrue(std::string(outEvent->name) == "lookup_scope", "tryEventAt preserves event name");
    expectTrue(fuse::profiler::tryEventAt(1u, outEvent), "tryEventAt succeeds for last event");
    expectTrue(outEvent->phase == fuse::profiler::EventPhase::End, "tryEventAt returns scope end");
    expectTrue(!fuse::profiler::tryEventAt(2u, outEvent), "tryEventAt fails past event count");
    expectTrue(outEvent->name == nullptr, "tryEventAt past count returns sentinel");
void testPreflightGuardState() {
    const fuse::profiler::ProfilerGuardPreflight resetPreflight = fuse::profiler::preflightGuardState();
    expectTrue(!resetPreflight.hasOpenScopes, "reset preflight has no open scopes");
    expectTrue(!resetPreflight.hasOpenAsyncFlows, "reset preflight has no open async flows");
    expectTrue(!resetPreflight.canEndAsyncFlow, "reset preflight cannot end async flow");
    expectTrue(resetPreflight.scopeDepth == 0u, "reset preflight scope depth is zero");
    expectTrue(resetPreflight.flowDepth == 0u, "reset preflight flow depth is zero");
        const fuse::profiler::ProfilerGuardPreflight scopedPreflight = fuse::profiler::preflightGuardState();
        expectTrue(scopedPreflight.hasOpenScopes, "scoped preflight marks open scopes");
        expectTrue(scopedPreflight.scopeDepth == 1u, "scoped preflight reports scope depth");
        const fuse::profiler::ProfilerGuardPreflight flowPreflight = fuse::profiler::preflightGuardState();
        expectTrue(flowPreflight.hasOpenAsyncFlows, "flow preflight marks open async flows");
        expectTrue(flowPreflight.canEndAsyncFlow, "flow preflight allows async finish");
        expectTrue(flowPreflight.openAsyncFlows == 1u, "flow preflight tracks open count");
        expectTrue(flowPreflight.flowDepth == 1u, "flow preflight reports flow depth");
        expectTrue(flowPreflight.hasUnmatchedAsyncFlows(), "flow preflight marks unmatched flows");
    const fuse::profiler::ProfilerGuardPreflight clearedPreflight = fuse::profiler::preflightGuardState();
    expectTrue(!clearedPreflight.hasOpenScopes, "post-scope preflight clears open scopes");
    expectTrue(!clearedPreflight.hasOpenAsyncFlows, "post-flow preflight clears open async flows");
    expectTrue(clearedPreflight.maxScopeDepth >= 1u, "preflight retains max scope depth");
    expectTrue(clearedPreflight.maxFlowDepth >= 1u, "preflight retains max flow depth");
    expectTrue(emptyPreflight.canExport, "empty export preflight allows export");
    expectTrue(emptyPreflight.bufferEmpty, "empty export preflight marks empty buffer");
    expectTrue(!emptyPreflight.hasEventsToExport(), "empty export preflight has no events");
    expectTrue(emptyPreflight.frameIndex == 0u, "empty export preflight reports frame index");
    expectTrue(!emptyPreflight.hasOpenScopes, "empty export preflight has no open scopes");
    expectTrue(!emptyPreflight.hasUnmatchedAsyncFlows, "empty export preflight has no unmatched flows");
    fuse::profiler::ChromeTraceExportPreflight unmatchedPreflight{};
        unmatchedPreflight = fuse::profiler::preflightChromeTraceExport();
        expectTrue(unmatchedPreflight.hasOpenScopes, "unmatched export preflight sees open scope");
    expectTrue(unmatchedPreflight.canExport, "unmatched-flow export preflight still allows export");
    expectTrue(!unmatchedPreflight.bufferEmpty, "unmatched export preflight sees recorded events");
    expectTrue(unmatchedPreflight.hasEventsToExport(), "unmatched export preflight has events");
    expectTrue(unmatchedPreflight.hasOpenAsyncFlows, "unmatched export preflight sees open flow");
    expectTrue(unmatchedPreflight.hasUnmatchedAsyncFlows,
    testPreflightGuardState();

// --- deepen additive from deepen-b16-profiler-preflights-479f ---
void testEmptyNameScopeGuard() {
void testEmptyNameFlowAndCounterGuards() {
    expectTrue(fuse::profiler::preflightBeginAsyncFlow("").emptyName,
    expectTrue(!fuse::profiler::preflightBeginAsyncFlow("").canBegin(),
    expectTrue(fuse::profiler::preflightEndAsyncFlow("").emptyName,
    expectTrue(!fuse::profiler::preflightEndAsyncFlow("").canEnd(),
    expectTrue(out.name == nullptr, "tryEventAt leaves out sentinel on failure");
    expectTrue(std::string(out.name) == "lookup_scope", "tryEventAt copies event name");
    expectTrue(!fuse::profiler::tryEventAt(99u, out), "tryEventAt false when out of range");
    const fuse::profiler::AsyncFlowEndPreflight orphanPreflight = fuse::profiler::preflightEndAsyncFlow("flow");
    expectTrue(orphanPreflight.orphanEnd, "end preflight marks orphan finish");
    expectTrue(!orphanPreflight.canEnd(), "end preflight rejects orphan finish");
        fuse::profiler::preflightBeginAsyncFlow("paired_flow");
    expectTrue(beginPreflight.canBegin(), "begin preflight accepts valid name");
    expectTrue(!beginPreflight.profilerDisabled, "begin preflight clears profilerDisabled");
    expectTrue(!beginPreflight.emptyName, "begin preflight clears emptyName");
    expectTrue(matchedPreflight.canEnd(), "end preflight accepts matched begin");
    expectTrue(!matchedPreflight.orphanEnd, "matched end preflight clears orphanEnd");
        fuse::profiler::preflightBeginAsyncFlow("ignored");
void testNestingStatePreflight() {
    const fuse::profiler::NestingStatePreflight initial = fuse::profiler::preflightNestingState();
        const fuse::profiler::NestingStatePreflight nested = fuse::profiler::preflightNestingState();
    const fuse::profiler::NestingStatePreflight finalState = fuse::profiler::preflightNestingState();
    expectTrue(emptyPreflight.canExport(), "empty buffer can export");
    expectTrue(emptyPreflight.bufferEmpty, "empty preflight marks buffer empty");
    expectTrue(emptyPreflight.eventCount == 0u, "empty preflight event count zero");
    expectTrue(emptyPreflight.exportableEventCount == 0u, "empty preflight exportable count zero");
    expectTrue(emptyPreflight.droppedEventCount == 0u, "empty preflight dropped count zero");
    const fuse::profiler::ChromeExportPreflight filledPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(filledPreflight.canExport(), "filled buffer can export");
    expectTrue(!filledPreflight.bufferEmpty, "filled preflight clears bufferEmpty");
    expectTrue(filledPreflight.eventCount == 3u, "filled preflight reports event count");
    expectTrue(filledPreflight.exportableEventCount == 3u, "filled preflight counts exportable events");
    expectTrue(filledPreflight.skippedInvalidNameCount == 0u, "filled preflight skips no valid names");
    const fuse::profiler::ChromeExportPreflight disabledPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(disabledPreflight.profilerDisabled, "disabled profiler marks export preflight");
    expectTrue(!disabledPreflight.canExport(), "disabled profiler rejects export preflight");
    const fuse::profiler::ChromeExportPreflight overflowPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(overflowPreflight.droppedEventCount == 1u, "export preflight reports dropped events");
    testNestingStatePreflight();

// --- deepen additive from deepen-b16-profiler-guards-5d2c ---
    expectTrue(emptyPreflight.canExport(), "empty buffer can still export");
    expectTrue(emptyPreflight.bufferEmpty, "preflight marks empty buffer");
    expectTrue(!emptyPreflight.hasStateWarnings(), "clean reset has no state warnings");
    expectTrue(!emptyPreflight.unbalancedScopeNesting, "reset leaves scope nesting balanced");
    expectTrue(!emptyPreflight.unbalancedFlowNesting, "reset leaves flow nesting balanced");
    expectTrue(!emptyPreflight.hasOpenAsyncFlows, "reset leaves no open async flows");
    const fuse::profiler::ChromeExportPreflight dirtyPreflight = fuse::profiler::preflightChromeExport();
    expectTrue(dirtyPreflight.canExport(), "unbalanced state can still export");
    expectTrue(!dirtyPreflight.bufferEmpty, "preflight sees recorded events");
    expectTrue(dirtyPreflight.hasStateWarnings(), "unmatched flow triggers state warnings");
    expectTrue(!dirtyPreflight.unbalancedScopeNesting, "ended scope is balanced");
    expectTrue(dirtyPreflight.unbalancedFlowNesting, "unmatched flow marks flow nesting unbalanced");
    expectTrue(dirtyPreflight.hasOpenAsyncFlows, "unmatched flow marks open async flows");

// --- deepen additive from deepen-b16-profiler-guards-5e0c ---
void testExportPreflightStubs() {
    expectTrue(!fuse::profiler::isValidProfileEvent(out), "tryEventAt out param unchanged on failure");
    expectTrue(!fuse::profiler::tryEventAt(99u, out), "tryEventAt false for out-of-range index");
void testIsValidProfileEventRejectsEmptyName() {
    testExportPreflightStubs();

// --- deepen additive from deepen-b16-profiler-export-preflight-9968 ---
void testChromeExportPreflightEmptyBuffer() {
    const fuse::profiler::ChromeExportPreflight preflight = fuse::profiler::preflightChromeTraceExport();
void testChromeExportPreflightWithEvents() {
void testChromeExportPreflightUnbalancedNesting() {
void testChromeExportPreflightDisabledProfiler() {
    testChromeExportPreflightEmptyBuffer();
    testChromeExportPreflightWithEvents();
    testChromeExportPreflightUnbalancedNesting();
    testChromeExportPreflightDisabledProfiler();

// --- deepen additive from deepen-b16-profiler-export-preflights-d73d ---
void testChromeTraceExportPreflightOnReset() {
void testChromeTraceExportPreflightUnbalancedNesting() {
               "tryFirstEvent copies scope name");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastEvent copies end phase");
               "tryLastEvent copies scope name");
    expectTrue(fuse::profiler::isValidProfileEvent(outEvent), "tryLastEvent output passes isValidProfileEvent");
    testChromeTraceExportPreflightOnReset();
    testChromeTraceExportPreflightUnbalancedNesting();

// --- deepen additive from deepen-b16-profiler-export-preflight-bcfd ---
               "tryLastEvent copies most recent scope name");
void testChromeTraceExportPreflightEmpty() {
    expectTrue(fuse::profiler::chromeTraceExportRejectReason()
void testChromeTraceExportPreflightClean() {
void testChromeTraceExportPreflightOpenFlowWarning() {
void testChromeTraceExportPreflightUnbalancedScope() {
    testChromeTraceExportPreflightEmpty();
    testChromeTraceExportPreflightClean();
    testChromeTraceExportPreflightOpenFlowWarning();
    testChromeTraceExportPreflightUnbalancedScope();

// --- deepen additive from deepen-b16-profiler-preflights-3a15 ---
void testIsValidEventNameGuards() {
    expectTrue(fuse::profiler::tryEventAt(0u, recorded), "recorded event is retrievable");
void testExportPreflightEmptyBuffer() {
void testExportPreflightWithEvents() {
void testExportPreflightUnbalancedWarnings() {
        const fuse::profiler::ChromeTraceExportPreflight dirtyPreflight =
        expectTrue(dirtyPreflight.canExport(), "unbalanced nesting still allows export");
        expectTrue(!dirtyPreflight.isClean(), "open scope and flow mark preflight dirty");
        expectTrue(dirtyPreflight.unbalancedScopeNesting, "preflight warns on active scope");
        expectTrue(dirtyPreflight.unbalancedFlowNesting, "preflight warns on active async flow");
        expectTrue(dirtyPreflight.hasOpenAsyncFlows, "preflight warns on open async flows");
        expectTrue(dirtyPreflight.eventCount >= 2u, "preflight counts events recorded before export");
    const fuse::profiler::ChromeTraceExportPreflight afterScope =
    const fuse::profiler::ChromeTraceExportPreflight afterReset =
void testExportPreflightDisabledProfiler() {
               "tryLastEvent output invalid on empty buffer");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastEvent returns most recent end");
               "tryLastEvent copies most recent event name");
    expectTrue(fuse::profiler::isValidProfileEvent(outEvent), "tryLastEvent output is valid");
    expectTrue(!fuse::profiler::tryLastEvent(outEvent), "tryLastEvent false after reset");
    expectTrue(outEvent.name == nullptr, "tryLastEvent clears output after reset");
    testExportPreflightEmptyBuffer();
    testExportPreflightWithEvents();
    testExportPreflightUnbalancedWarnings();
    testExportPreflightDisabledProfiler();

// --- deepen additive from deepen-fuse-b16-profiler-11c2 ---
void testEventNameValidationPreflight() {
    fuse::profiler::EventNameRejectReason reason = fuse::profiler::EventNameRejectReason::None;
    expectTrue(fuse::profiler::tryValidateEventName("scope", reason),
               "tryValidateEventName succeeds for valid name");
    expectTrue(reason == fuse::profiler::EventNameRejectReason::None, "valid name reject reason is None");
    expectTrue(std::string(fuse::profiler::eventNameRejectReasonLabel(reason)) == "none",
    expectTrue(!fuse::profiler::tryValidateEventName(nullptr, reason),
               "tryValidateEventName rejects null name");
    expectTrue(reason == fuse::profiler::EventNameRejectReason::Null, "null name reject reason is Null");
    expectTrue(std::string(fuse::profiler::eventNameRejectReasonLabel(reason)) == "null",
    expectTrue(!fuse::profiler::tryValidateEventName("", reason),
               "tryValidateEventName rejects empty name");
    expectTrue(reason == fuse::profiler::EventNameRejectReason::Empty, "empty name reject reason is Empty");
    expectTrue(std::string(fuse::profiler::eventNameRejectReasonLabel(reason)) == "empty",
void testProfilerStatePreflight() {
    fuse::profiler::NestingStateRejectReason reason = fuse::profiler::NestingStateRejectReason::None;
    expectTrue(fuse::profiler::preflightProfilerState(&reason),
               "preflightProfilerState succeeds after reset");
    expectTrue(reason == fuse::profiler::NestingStateRejectReason::None, "balanced state reject reason is None");
        expectTrue(!fuse::profiler::preflightProfilerState(&reason),
                   "preflightProfilerState rejects active scope");
        expectTrue(reason == fuse::profiler::NestingStateRejectReason::UnbalancedScopeNesting,
        expectTrue(std::string(fuse::profiler::nestingStateRejectReasonLabel(reason))
               "preflightProfilerState rejects open async flow");
    expectTrue(reason == fuse::profiler::NestingStateRejectReason::UnbalancedFlowNesting,
               "preflightProfilerState succeeds after flow pair");
    fuse::profiler::ChromeTraceExportRejectReason reason =
        fuse::profiler::ChromeTraceExportRejectReason::None;
    expectTrue(fuse::profiler::preflightChromeTraceExport(&reason),
               "preflightChromeTraceExport succeeds on empty buffer");
    expectTrue(reason == fuse::profiler::ChromeTraceExportRejectReason::None,
        expectTrue(!fuse::profiler::preflightChromeTraceExport(&reason),
                   "preflightChromeTraceExport rejects active scope");
        expectTrue(reason == fuse::profiler::ChromeTraceExportRejectReason::UnbalancedScopeNesting,
               "preflightChromeTraceExport rejects open async flow");
    expectTrue(reason == fuse::profiler::ChromeTraceExportRejectReason::UnbalancedFlowNesting,
    expectTrue(std::string(fuse::profiler::chromeTraceExportRejectReasonLabel(reason))
    expectTrue(!fuse::profiler::tryExportChromeTraceJson(guardedJson, &reason),
               "tryExportChromeTraceJson rejects unbalanced state");
    expectTrue(guardedJson.empty(), "tryExportChromeTraceJson clears output on rejection");
               "tryExportChromeTraceJson reports unbalanced flow nesting");
    expectTrue(fuse::profiler::tryExportChromeTraceJson(guardedJson, &reason),
               "tryExportChromeTraceJson succeeds on balanced state");
    expectTrue(!guardedJson.empty(), "tryExportChromeTraceJson returns non-empty json");
               "tryExportChromeTraceJson preserves scope name");
void testEventLookupPreflightStubs() {
    fuse::profiler::EventLookupRejectReason reason = fuse::profiler::EventLookupRejectReason::None;
    expectTrue(!fuse::profiler::tryCanLookupEventAt(0u, reason),
               "tryCanLookupEventAt false on empty buffer");
    expectTrue(reason == fuse::profiler::EventLookupRejectReason::EmptyBuffer,
    expectTrue(std::string(fuse::profiler::eventLookupRejectReasonLabel(reason)) == "empty_buffer",
    expectTrue(fuse::profiler::tryCanLookupEventAt(1u, reason),
               "tryCanLookupEventAt succeeds for last event");
    expectTrue(reason == fuse::profiler::EventLookupRejectReason::None,
    expectTrue(!fuse::profiler::tryCanLookupEventAt(2u, reason),
               "tryCanLookupEventAt rejects out-of-range index");
    expectTrue(reason == fuse::profiler::EventLookupRejectReason::OutOfRange,
    expectTrue(std::string(fuse::profiler::eventLookupRejectReasonLabel(reason)) == "out_of_range",
void testOpenAsyncFlowCrossThreadPreflight() {
    fuse::profiler::NestingStateRejectReason nestingReason = fuse::profiler::NestingStateRejectReason::None;
    fuse::profiler::ChromeTraceExportRejectReason exportReason =
        expectTrue(!fuse::profiler::preflightProfilerState(&nestingReason),
        expectTrue(nestingReason == fuse::profiler::NestingStateRejectReason::OpenAsyncFlows,
        expectTrue(!fuse::profiler::preflightChromeTraceExport(&exportReason),
        expectTrue(exportReason == fuse::profiler::ChromeTraceExportRejectReason::OpenAsyncFlows,
void testUnbalancedFlowNestingExportPreflight() {
               "preflightProfilerState rejects unmatched outer flow");
    expectTrue(nestingReason == fuse::profiler::NestingStateRejectReason::UnbalancedFlowNesting,
               "preflightChromeTraceExport rejects unmatched outer flow");
    expectTrue(exportReason == fuse::profiler::ChromeTraceExportRejectReason::UnbalancedFlowNesting,
    expectTrue(fuse::profiler::preflightChromeTraceExport(&exportReason),
               "preflightChromeTraceExport succeeds after flow balance restored");
    testEventNameValidationPreflight();
    testProfilerStatePreflight();
    testEventLookupPreflightStubs();
    testOpenAsyncFlowCrossThreadPreflight();
    testUnbalancedFlowNestingExportPreflight();

// --- deepen additive from deepen-b16-profiler-export-preflights-d715 ---
void testRingBufferCapacityGuard() {
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastEvent returns last end event");
               "tryLastEvent copies last event name");
void testChromeTraceExportPreflightUnbalancedScopeNesting() {
    const fuse::profiler::ChromeTraceExportPreflight afterEnd = fuse::profiler::preflightChromeTraceExport();
void testChromeTraceExportPreflightOpenAsyncFlows() {
void testChromeTraceExportPreflightUnbalancedFlowNesting() {
    expectTrue(fuse::profiler::preflightChromeTraceExport().isTraceComplete(),
void testChromeTraceExportPreflightAfterValidCapture() {
    testChromeTraceExportPreflightUnbalancedScopeNesting();
    testChromeTraceExportPreflightOpenAsyncFlows();
    testChromeTraceExportPreflightUnbalancedFlowNesting();
    testChromeTraceExportPreflightAfterValidCapture();

// --- deepen additive from deepen-b16-profiler-preflights-07a7 ---
void testIsValidEventNamePreflight() {
void testRecordEntryPointPreflights() {
void testCanEndAsyncFlowPreflightWithOpenFlow() {
void testProfilerGuardStateBalancedPreflight() {
void testTryLastEventPreflight() {
void testExportPreflights() {
void testPreflightsRejectInvalidNamesWithoutRecording() {
    testIsValidEventNamePreflight();
    testRecordEntryPointPreflights();
    testCanEndAsyncFlowPreflightWithOpenFlow();
    testProfilerGuardStateBalancedPreflight();
    testTryLastEventPreflight();
    testExportPreflights();
    testPreflightsRejectInvalidNamesWithoutRecording();

// --- deepen additive from deepen-b16-profiler-guards-5521 ---
void testIsProfilerGuardStateBalanced() {
    expectTrue(fuse::profiler::tryEventAt(0u, recorded), "recorded event passes tryEventAt");

// --- deepen additive from deepen-b16-profiler-preflight-guards-b702 ---
void testIsValidProfilerNamePreflight() {
    expectTrue(!fuse::profiler::tryExportChromeTraceJson(json),
               "tryExportChromeTraceJson false when buffer is empty");
               "empty tryExport emits zero trace events");
    expectTrue(fuse::profiler::tryExportChromeTraceJson(json), "tryExportChromeTraceJson true with events");
               "tryExport includes scope name");
               "tryExport includes counter track name");
               "populated tryExport emits trace events");
    testIsValidProfilerNamePreflight();

// --- deepen additive from deepen-b16-profiler-guards-ca8d ---
void testEventNameValidPreflight() {
void testExportPreflightGuards() {
void testGuardStateBalancedIntrospection() {
    testEventNameValidPreflight();
    testExportPreflightGuards();

// --- deepen additive from deepen-b16-profiler-preflights-acf4 ---
void testEventLookupPreflightGuards() {
    expectTrue(!fuse::profiler::isEventLookupPreflightOk(0u),
    expectTrue(!fuse::profiler::isEventLookupPreflightOk(99u),
    expectTrue(fuse::profiler::isEventLookupPreflightOk(0u),
    expectTrue(fuse::profiler::isEventLookupPreflightOk(1u),
    expectTrue(!fuse::profiler::isEventLookupPreflightOk(2u),
void testProfilerNestingPreflightGuards() {
    expectTrue(fuse::profiler::isProfilerNestingPreflightOk(),
    expectTrue(fuse::profiler::isChromeExportPreflightOk(),
        expectTrue(!fuse::profiler::isProfilerNestingPreflightOk(),
        expectTrue(!fuse::profiler::isChromeExportPreflightOk(),
void testChromeExportPreflightWithUnmatchedFlow() {
void testEmptyNameDoesNotPassProfileEventPreflight() {
    testEventLookupPreflightGuards();
    testProfilerNestingPreflightGuards();
    testChromeExportPreflightWithUnmatchedFlow();
    testEmptyNameDoesNotPassProfileEventPreflight();

// --- deepen additive from deepen-b16-profiler-preflights-3f2f ---
void testProfileScopePreflightGuard() {
    expectTrue(nullPreflight.emptyName, "preflight marks null scope name");
    expectTrue(!nullPreflight.canEnter(), "preflight rejects null scope name");
    expectTrue(emptyPreflight.emptyName, "preflight marks empty scope name");
    expectTrue(!emptyPreflight.canEnter(), "preflight rejects empty scope name");
        fuse::profiler::preflightProfileScope("disabled_scope");
    expectTrue(disabledPreflight.disabled, "preflight marks disabled profiler");
    expectTrue(!disabledPreflight.canEnter(), "preflight rejects disabled profiler");
        fuse::profiler::preflightProfileScope("enabled_scope");
    expectTrue(validPreflight.canEnter(), "preflight accepts valid enabled scope");
    expectTrue(!validPreflight.emptyName, "valid preflight clears emptyName");
    expectTrue(!validPreflight.disabled, "valid preflight clears disabled");
void testAsyncFlowPreflightGuards() {
    const fuse::profiler::AsyncFlowPreflight orphanPreflight =
        fuse::profiler::preflightEndAsyncFlow("orphan_flow");
    expectTrue(orphanPreflight.orphanEnd, "preflight marks orphan flow end");
    expectTrue(!orphanPreflight.canEnd(), "preflight rejects orphan flow end");
    const fuse::profiler::AsyncFlowPreflight beginPreflight =
    expectTrue(beginPreflight.canBegin(), "preflight accepts valid flow begin");
    const fuse::profiler::AsyncFlowPreflight openEndPreflight =
    expectTrue(!openEndPreflight.orphanEnd, "preflight clears orphanEnd with open flow");
    expectTrue(openEndPreflight.canEnd(), "preflight accepts flow end with open flow");
    const fuse::profiler::AsyncFlowPreflight emptyBeginPreflight =
    expectTrue(emptyBeginPreflight.emptyName, "preflight marks empty flow begin name");
    expectTrue(!emptyBeginPreflight.canBegin(), "preflight rejects empty flow begin");
void testNestingPreflightGuard() {
    const fuse::profiler::NestingPreflight resetPreflight = fuse::profiler::preflightNesting();
    expectTrue(resetPreflight.isBalanced(), "reset nesting preflight is balanced");
    expectTrue(resetPreflight.scopeBalanced, "reset scope nesting is balanced");
    expectTrue(resetPreflight.flowBalanced, "reset flow nesting is balanced");
    expectTrue(!resetPreflight.hasOpenFlows, "reset has no open flows");
        const fuse::profiler::NestingPreflight activePreflight = fuse::profiler::preflightNesting();
        expectTrue(!activePreflight.scopeBalanced, "active scope reports unbalanced nesting");
        expectTrue(activePreflight.scopeDepth == 1u, "preflight reports scope depth");
        const fuse::profiler::NestingPreflight flowPreflight = fuse::profiler::preflightNesting();
        expectTrue(!flowPreflight.flowBalanced, "open flow reports unbalanced flow nesting");
        expectTrue(flowPreflight.hasOpenFlows, "open flow reports hasOpenFlows");
        expectTrue(flowPreflight.openFlowCount == 1u, "preflight reports open flow count");
        expectTrue(flowPreflight.flowDepth == 1u, "preflight reports flow depth");
        expectTrue(fuse::profiler::preflightNesting().flowBalanced,
    expectTrue(fuse::profiler::preflightNesting().isBalanced(),
void testExportPreflightGuard() {
    const fuse::profiler::ExportPreflight emptyPreflight = fuse::profiler::preflightExport();
    expectTrue(emptyPreflight.canExport(), "export preflight allows empty buffer export");
    expectTrue(emptyPreflight.emptyBuffer, "export preflight marks empty buffer");
    expectTrue(!emptyPreflight.hasExportableEvents(), "empty buffer has no exportable events");
    expectTrue(emptyPreflight.exportableEventCount == 0u, "empty buffer exportable count is zero");
    const fuse::profiler::ExportPreflight filledPreflight = fuse::profiler::preflightExport();
    expectTrue(filledPreflight.hasExportableEvents(), "filled buffer has exportable events");
    expectTrue(filledPreflight.exportableEventCount == filledPreflight.bufferedEventCount,
    expectTrue(filledPreflight.skippedInvalidNames == 0u, "valid buffer skips no names");
    const fuse::profiler::ExportPreflight disabledPreflight = fuse::profiler::preflightExport();
    expectTrue(disabledPreflight.disabled, "disabled profiler marks export preflight disabled");
    expectTrue(!disabledPreflight.canExport(), "disabled profiler rejects export");
void testEventLookupPreflightGuard() {
    const fuse::profiler::EventLookupPreflight emptyPreflight = fuse::profiler::preflightEventAt(0u);
    expectTrue(emptyPreflight.emptyBuffer, "empty buffer preflight marks emptyBuffer");
    expectTrue(emptyPreflight.indexOutOfRange, "empty buffer preflight marks indexOutOfRange");
    expectTrue(!emptyPreflight.canLookup(), "empty buffer preflight rejects lookup");
    const fuse::profiler::EventLookupPreflight lastEmptyPreflight = fuse::profiler::preflightLastEvent();
    expectTrue(lastEmptyPreflight.emptyBuffer, "last-event preflight marks empty buffer");
    expectTrue(!lastEmptyPreflight.canReadValidEvent(), "last-event preflight invalid on empty buffer");
    const fuse::profiler::EventLookupPreflight firstPreflight = fuse::profiler::preflightEventAt(0u);
    expectTrue(firstPreflight.canLookup(), "preflight accepts first event index");
    expectTrue(firstPreflight.canReadValidEvent(), "preflight accepts valid first event");
    expectTrue(!firstPreflight.invalidEvent, "valid event clears invalidEvent flag");
    const fuse::profiler::EventLookupPreflight lastPreflight = fuse::profiler::preflightLastEvent();
    expectTrue(lastPreflight.canReadValidEvent(), "last-event preflight accepts valid event");
    expectTrue(lastPreflight.index == 1u, "last-event preflight points at last index");
    const fuse::profiler::EventLookupPreflight oobPreflight = fuse::profiler::preflightEventAt(99u);
    expectTrue(oobPreflight.indexOutOfRange, "out-of-range preflight marks indexOutOfRange");
    expectTrue(!oobPreflight.canLookup(), "out-of-range preflight rejects lookup");
    testProfileScopePreflightGuard();
    testAsyncFlowPreflightGuards();
    testNestingPreflightGuard();
    testExportPreflightGuard();
    testEventLookupPreflightGuard();

// --- deepen additive from deepen-profiler-b16-guards-10ba ---
void testEventNameAtAndTryEventPhaseGuard() {
    expectTrue(!fuse::profiler::tryEventPhaseAt(0u, outPhase), "tryEventPhaseAt false on empty buffer");
               "tryEventPhaseAt resets phase on empty buffer");
    expectTrue(fuse::profiler::tryEventPhaseAt(0u, outPhase), "tryEventPhaseAt true for begin");
    expectTrue(outPhase == fuse::profiler::EventPhase::Begin, "tryEventPhaseAt copies begin phase");
    expectTrue(fuse::profiler::tryEventPhaseAt(1u, outPhase), "tryEventPhaseAt true for end");
    expectTrue(outPhase == fuse::profiler::EventPhase::End, "tryEventPhaseAt copies end phase");
    expectTrue(!fuse::profiler::tryEventPhaseAt(2u, outPhase), "tryEventPhaseAt false past count");
void testCountEventsWithPhaseAndFindFirstGuard() {
void testIsFlowOpenCountAttachedGuard() {
void testChromeTraceExportPreflightStructuralBalance() {
    expectTrue(emptyPreflight.isBufferStructurallyBalanced(),
    expectTrue(emptyPreflight.hasBalancedScopeEventsInBuffer(),
    expectTrue(emptyPreflight.hasBalancedAsyncFlowEventsInBuffer(),
    expectTrue(!emptyPreflight.canExportNonEmptyTrace(),
    expectTrue(!emptyPreflight.bufferFull, "empty buffer is not full");
    const fuse::profiler::ChromeTraceExportPreflight openPreflight =
    expectTrue(!openPreflight.hasBalancedAsyncFlowEventsInBuffer(),
    expectTrue(!openPreflight.isBufferStructurallyBalanced(),
    expectTrue(openPreflight.asyncFlowStartEventCount == 2u,
    expectTrue(openPreflight.asyncFlowFinishEventCount == 1u,
    testChromeTraceExportPreflightStructuralBalance();

// --- deepen additive from deepen-b16-profiler-guards-fb79 ---
void testRingSaturationAndDroppedEventCountGuard() {
    expectTrue(outEvent.name == nullptr, "tryExportableEventAt clears output on empty buffer");
               "tryExportableEventAt true for exportable begin");
               "tryExportableEventAt copies exportable begin phase");
               "tryExportableEventAt copies exportable event name");
    expectTrue(outEvent.name == nullptr, "tryExportableEventAt clears output when out of range");
void testExportableFirstAndLastEventIndexGuard() {
void testTryFirstAndLastExportableEventGuard() {
    expectTrue(!fuse::profiler::tryFirstExportableEvent(outEvent),
               "tryFirstExportableEvent false on empty buffer");
    expectTrue(!fuse::profiler::tryLastExportableEvent(outEvent),
               "tryLastExportableEvent false on empty buffer");
    expectTrue(fuse::profiler::tryFirstExportableEvent(outEvent),
               "tryFirstExportableEvent true after recording");
               "tryFirstExportableEvent copies begin phase");
               "tryFirstExportableEvent copies first scope name");
    expectTrue(fuse::profiler::tryLastExportableEvent(outEvent),
               "tryLastExportableEvent true after recording");
               "tryLastExportableEvent copies end phase");
               "tryLastExportableEvent copies last scope name");
void testChromeTraceExportPreflightRingSaturated() {
void testChromeTraceExportPreflightNestingWarnings() {
        expectTrue(activePreflight.hasNestingWarnings(),
        expectTrue(!activePreflight.flowDepthDetached,
    const fuse::profiler::ChromeTraceExportPreflight detachedPreflight =
    expectTrue(detachedPreflight.hasNestingWarnings(),
    expectTrue(detachedPreflight.flowDepthDetached,
    expectTrue(detachedPreflight.hasUnbalancedNesting(),
    testChromeTraceExportPreflightRingSaturated();
    testChromeTraceExportPreflightNestingWarnings();

// --- deepen additive from deepen-b16-profiler-guards-739f ---
void testEventIndexBoundaryGuards() {
void testTryEventPhaseAtGuard() {
    expectTrue(!fuse::profiler::tryEventPhaseAt(0u, phase), "tryEventPhaseAt false on empty buffer");
    expectTrue(fuse::profiler::tryEventPhaseAt(0u, phase), "tryEventPhaseAt true for begin event");
    expectTrue(phase == fuse::profiler::EventPhase::Begin, "tryEventPhaseAt copies begin phase");
    expectTrue(fuse::profiler::tryEventPhaseAt(1u, phase), "tryEventPhaseAt true for end event");
    expectTrue(phase == fuse::profiler::EventPhase::End, "tryEventPhaseAt copies end phase");
    expectTrue(!fuse::profiler::tryEventPhaseAt(2u, phase), "tryEventPhaseAt false past event count");
void testCountEventsByPhaseGuard() {
void testCanEndAsyncFlowGuard() {
void testHasActiveScopesAndFlowsGuards() {
void testRingBufferOverflowGuards() {
void testNonExportableEventCountGuard() {
void testChromeTraceExportPreflightExportWarnings() {
        const fuse::profiler::ChromeTraceExportPreflight activeScope = fuse::profiler::preflightChromeTraceExport();
    const fuse::profiler::ChromeTraceExportPreflight closed = fuse::profiler::preflightChromeTraceExport();
    testChromeTraceExportPreflightExportWarnings();

// --- deepen additive from deepen-b16-profiler-guards-5e82 ---
void testCountEventsWithPhaseGuard() {
    expectTrue(!fuse::profiler::tryFindEventByName(nullptr, 0u, outIndex, outEvent),
               "tryFindEventByName rejects null name");
               "tryFindEventByName clears index for null name");
    expectTrue(!fuse::profiler::tryFindEventByName("", 0u, outIndex, outEvent),
               "tryFindEventByName rejects empty name");
    expectTrue(!fuse::profiler::tryFindEventByName("missing", 0u, outIndex, outEvent),
               "tryFindEventByName false on empty buffer");
    expectTrue(fuse::profiler::tryFindEventByName("lookup_scope", 0u, outIndex, outEvent),
               "tryFindEventByName locates scope begin");
    expectTrue(outIndex == 0u, "tryFindEventByName returns first matching index");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryFindEventByName copies begin phase");
    expectTrue(fuse::profiler::tryFindEventByName("lookup_counter", 0u, outIndex, outEvent),
               "tryFindEventByName locates counter sample");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Counter, "tryFindEventByName copies counter phase");
    expectTrue(fuse::profiler::tryFindEventByName("lookup_scope", 1u, outIndex, outEvent),
               "tryFindEventByName finds second occurrence from start index");
    expectTrue(outIndex == 2u, "tryFindEventByName skips earlier matches with start index");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryFindEventByName finds scope end");
               "tryFindEventByName false when name not present");
    expectTrue(!fuse::profiler::tryFindEventByName("lookup_scope", 99u, outIndex, outEvent),
               "tryFindEventByName false when start index out of range");
void testReconcileDetachedFlowDepthGuard() {
void testChromeTraceExportPreflightExtendedFields() {
    expectTrue(!disabledPreflight.isExportReady(), "preflight isExportReady false when disabled");
    expectTrue(!disabledPreflight.canExport(), "preflight canExport false when disabled");
    testChromeTraceExportPreflightExtendedFields();

// --- deepen additive from deepen-b16-profiler-guards-1296 ---
void testTryValidateEventNameGuard() {
               "tryValidateEventName rejects null");
    expectTrue(reason == fuse::profiler::EventNameRejectReason::Null, "null name reason is Null");
               "tryValidateEventName rejects empty string");
    expectTrue(reason == fuse::profiler::EventNameRejectReason::Empty, "empty name reason is Empty");
    expectTrue(fuse::profiler::tryValidateEventName("valid_scope", reason),
               "tryValidateEventName accepts non-empty name");
    expectTrue(reason == fuse::profiler::EventNameRejectReason::None, "valid name reason is None");
void testTryCanLookupEventAtGuard() {
    expectTrue(!fuse::profiler::tryCanLookupEventAt(0u, reason), "tryCanLookupEventAt false on empty buffer");
    expectTrue(fuse::profiler::tryCanLookupEventAt(0u, reason), "tryCanLookupEventAt true for first event");
    expectTrue(reason == fuse::profiler::EventLookupRejectReason::None, "valid lookup reason is None");
    expectTrue(!fuse::profiler::tryCanLookupEventAt(2u, reason), "tryCanLookupEventAt false past count");
void testPreflightProfilerStateGuard() {
    expectTrue(fuse::profiler::preflightProfilerState(&reason), "preflightProfilerState true on reset");
    expectTrue(reason == fuse::profiler::NestingStateRejectReason::None, "balanced state reason is None");
                   "preflightProfilerState false inside active scope");
    expectTrue(!fuse::profiler::preflightProfilerState(&reason), "open flow reports unbalanced state");
    expectTrue(reason == fuse::profiler::NestingStateRejectReason::OpenAsyncFlows,
    expectTrue(fuse::profiler::preflightProfilerState(&reason), "paired flow restores balanced state");
void testPreflightProfilerStateDetachedFlow() {
    expectTrue(!fuse::profiler::preflightProfilerState(&reason), "preflightProfilerState false after detach");
    expectTrue(reason == fuse::profiler::NestingStateRejectReason::FlowDepthDetached,
    expectTrue(std::string(fuse::profiler::nestingStateRejectReasonLabel(reason)) == "flow_depth_detached",
void testPreflightChromeTraceNestingGuard() {
    fuse::profiler::ChromeTraceExportRejectReason reason = fuse::profiler::ChromeTraceExportRejectReason::None;
    expectTrue(!fuse::profiler::preflightChromeTraceNesting(&reason),
               "preflightChromeTraceNesting false on empty buffer");
    expectTrue(reason == fuse::profiler::ChromeTraceExportRejectReason::NoExportableEvents,
    expectTrue(fuse::profiler::preflightChromeTraceNesting(&reason),
               "preflightChromeTraceNesting true after balanced scope");
               "preflightChromeTraceNesting false with open flow");
    expectTrue(reason == fuse::profiler::ChromeTraceExportRejectReason::OpenAsyncFlows,
void testPreflightChromeTraceNestingDisabledProfiler() {
               "preflightChromeTraceNesting false when disabled");
    expectTrue(reason == fuse::profiler::ChromeTraceExportRejectReason::ProfilerDisabled,
void testTryExportChromeTraceJsonGuard() {
               "tryExportChromeTraceJson false on empty buffer");
    expectTrue(guardedJson.empty(), "tryExportChromeTraceJson clears output on reject");
               "tryExportChromeTraceJson true after balanced recording");
void testTryExportChromeTraceJsonRejectsOpenFlow() {
               "tryExportChromeTraceJson false with open async flow");
    testPreflightProfilerStateGuard();
    testPreflightProfilerStateDetachedFlow();
    testPreflightChromeTraceNestingGuard();
    testPreflightChromeTraceNestingDisabledProfiler();

// --- deepen additive from deepen-b16-profiler-guards-3935 ---
void testRejectedInvalidNameCountGuard() {
    expectTrue(!fuse::profiler::hasRejectedInvalidNames(),
    expectTrue(fuse::profiler::hasRejectedInvalidNames(), "hasRejectedInvalidNames true after rejects");
    expectTrue(!fuse::profiler::hasRejectedInvalidNames(), "reset clears hasRejectedInvalidNames");
void testRemainingEventCapacityGuard() {
void testFindEventIndexAndCountByPhaseGuard() {
void testTryFindEventByScopeIdGuard() {
    expectTrue(!fuse::profiler::tryFindEventByScopeId(1u, outEvent),
               "tryFindEventByScopeId false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryFindEventByScopeId clears output on empty buffer");
    expectTrue(fuse::profiler::tryFindEventByScopeId(scopeId, outEvent),
               "tryFindEventByScopeId true for recorded scope id");
               "tryFindEventByScopeId returns first matching event");
               "tryFindEventByScopeId copies matching scope name");
    expectTrue(fuse::profiler::tryFindEventByScopeId(flowId, flowEvent),
               "tryFindEventByScopeId true for distinct async flow id");
               "tryFindEventByScopeId finds flow start by flow id");
               "tryFindEventByScopeId preserves flow name");
void testTryEventAtReverseGuard() {
    expectTrue(!fuse::profiler::tryEventAtReverse(0u, outEvent),
               "tryEventAtReverse false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryEventAtReverse clears output on empty buffer");
    expectTrue(fuse::profiler::tryEventAtReverse(0u, outEvent), "tryEventAtReverse true for newest event");
    expectTrue(fuse::profiler::tryEventAtReverse(1u, outEvent), "tryEventAtReverse true for prior event");
    expectTrue(fuse::profiler::tryEventAtReverse(2u, outEvent), "tryEventAtReverse true for oldest event");
    expectTrue(!fuse::profiler::tryEventAtReverse(3u, outEvent),
               "tryEventAtReverse false when reverse index is out of range");
void testChromeTraceExportPreflightGuardDiagnostics() {
    expectTrue(preflight.hasRejectedInvalidNames, "preflight marks rejected invalid names");
    testChromeTraceExportPreflightGuardDiagnostics();

// --- deepen additive from deepen-b16-profiler-guards-2ba8 ---
void testBlankWhitespaceNameGuards() {
void testPhaseLookupGuards() {
    expectTrue(!fuse::profiler::tryFindFirstEventWithPhase(fuse::profiler::EventPhase::Begin, outEvent),
               "tryFindFirstEventWithPhase false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryFindFirstEventWithPhase clears output on empty buffer");
    expectTrue(fuse::profiler::tryFindFirstEventWithPhase(fuse::profiler::EventPhase::FlowStart, outEvent),
               "tryFindFirstEventWithPhase true for flow start");
void testHasResidualFlowNestingDepthGuard() {
void testChromeTraceExportPreflightCleanTrace() {
    const fuse::profiler::ChromeTraceExportPreflight openPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!openPreflight.canExportCleanTrace(), "open flow fails clean export preflight");
    expectTrue(openPreflight.hasUnbalancedNesting(), "open flow marks unbalanced nesting");
    expectTrue(openPreflight.wouldExportEmptyTrace() == false, "open flow still has exportable events");
void testChromeTraceExportPreflightWouldExportEmpty() {
    expectTrue(emptyPreflight.wouldExportEmptyTrace(), "empty buffer would export empty trace");
    expectTrue(emptyPreflight.canExportCleanTrace(), "empty balanced buffer is clean export");
    expectTrue(!emptyPreflight.bufferFull, "empty preflight is not buffer full");
    const fuse::profiler::ChromeTraceExportPreflight blankPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(blankPreflight.wouldExportEmptyTrace(), "blank-name attempts still export empty trace");
    expectTrue(blankPreflight.eventCount == 0u, "blank-name attempts do not change event count");
    testChromeTraceExportPreflightCleanTrace();
    testChromeTraceExportPreflightWouldExportEmpty();

// --- deepen additive from deepen-b16-profiler-guards-c0f6 ---
void testWhitespaceOnlyNameGuards() {
void testFindFirstEventIndexByPhaseGuard() {
    expectTrue(!fuse::profiler::tryFindFirstEventByPhase(fuse::profiler::EventPhase::Counter, outEvent),
               "tryFindFirstEventByPhase false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryFindFirstEventByPhase clears output on empty buffer");
    expectTrue(fuse::profiler::tryFindFirstEventByPhase(fuse::profiler::EventPhase::Counter, outEvent),
               "tryFindFirstEventByPhase true for counter phase");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Counter, "tryFindFirstEventByPhase copies phase");
               "tryFindFirstEventByPhase copies counter track name");
void testChromeTraceExportPreflightPhaseCounts() {
void testChromeTraceExportPreflightBufferedFlowImbalance() {
void testChromeTraceExportPreflightOrphanCount() {
void testMixedWhitespaceAndValidNameGuards() {
    testChromeTraceExportPreflightPhaseCounts();
    testChromeTraceExportPreflightBufferedFlowImbalance();
    testChromeTraceExportPreflightOrphanCount();

// --- deepen additive from deepen-b16-profiler-guards-dcff ---
void testIsEmptyEventNameGuard() {
void testAsyncFlowBeginEndPreflights() {
    expectTrue(disabledBegin.profilerDisabled == false, "preflightBegin captures state before disable");
    const fuse::profiler::AsyncFlowBeginPreflight disabledAfter =
    expectTrue(disabledAfter.profilerDisabled, "preflightBegin marks disabled profiler");
    expectTrue(!disabledAfter.canBegin(), "preflightBegin rejects disabled profiler");
    expectTrue(emptyBegin.emptyName, "preflightBegin marks empty name");
    expectTrue(!emptyBegin.canBegin(), "preflightBegin rejects empty name");
    const fuse::profiler::AsyncFlowEndPreflight orphanEnd = fuse::profiler::preflightEndAsyncFlow("flow");
    expectTrue(orphanEnd.orphanEnd, "preflightEnd marks orphan finish");
    expectTrue(!orphanEnd.canEnd(), "preflightEnd rejects orphan finish");
        fuse::profiler::preflightEndAsyncFlow("paired_preflight");
    expectTrue(pairedEnd.canEnd(), "preflightEnd accepts paired finish");
    expectTrue(!pairedEnd.orphanEnd, "preflightEnd clears orphan flag for paired finish");
    const fuse::profiler::NestingStatePreflight resetPreflight = fuse::profiler::preflightNestingState();
    expectTrue(resetPreflight.isClean(), "reset nesting preflight is clean");
    expectTrue(resetPreflight.scopeBalanced, "reset scope nesting balanced");
    expectTrue(resetPreflight.flowBalanced, "reset flow nesting balanced");
        const fuse::profiler::NestingStatePreflight activePreflight = fuse::profiler::preflightNestingState();
        expectTrue(!activePreflight.isClean(), "active scope/flow preflight is not clean");
        expectTrue(activePreflight.scopeDepth == 1u, "nesting preflight reports scope depth");
        expectTrue(activePreflight.flowDepth == 1u, "nesting preflight reports flow depth");
        expectTrue(activePreflight.openAsyncFlows == 1u, "nesting preflight reports open flows");
    const fuse::profiler::NestingStatePreflight closedPreflight = fuse::profiler::preflightNestingState();
    expectTrue(closedPreflight.isClean(), "nesting preflight clean after teardown");
void testEventIndexLookupGuards() {
               "tryExportableEventAt true for exportable end");
    expectTrue(emptyPreflight.firstEventIndex == fuse::profiler::kInvalidEventIndex,
    expectTrue(emptyPreflight.lastEventIndex == fuse::profiler::kInvalidEventIndex,
    expectTrue(emptyPreflight.ringCapacity == fuse::profiler::ringCapacity(),
    expectTrue(emptyPreflight.droppedEventCount == 0u, "preflight dropped count zero on reset");
    expectTrue(!emptyPreflight.isBufferFull, "preflight buffer not full on reset");
    expectTrue(!emptyPreflight.canExportTrace(), "preflight canExportTrace false on empty buffer");
    expectTrue(!emptyPreflight.hasExportWarnings(), "preflight hasExportWarnings false on reset");
    const fuse::profiler::ChromeTraceExportPreflight filledPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(filledPreflight.firstEventIndex == 0u, "preflight firstEventIndex zero after recording");
    expectTrue(filledPreflight.lastEventIndex == 1u, "preflight lastEventIndex points at scope end");
    expectTrue(filledPreflight.canExportTrace(), "preflight canExportTrace true with exportable events");
    expectTrue(!filledPreflight.hasExportWarnings(), "preflight hasExportWarnings false after balanced scope");
void testDroppedEventCountGuard() {
    testAsyncFlowBeginEndPreflights();

// --- deepen additive from deepen-b16-profiler-guards-7793 ---
void testWouldRecordEventNameGuard() {
void testInvalidEventCountAndIndexGuards() {
    expectTrue(phase == fuse::profiler::EventPhase::Begin, "tryEventPhaseAt clears phase on failure");
    expectTrue(fuse::profiler::tryEventPhaseAt(1u, phase), "tryEventPhaseAt true for counter event");
    expectTrue(phase == fuse::profiler::EventPhase::Counter, "tryEventPhaseAt copies counter phase");
    expectTrue(fuse::profiler::tryEventPhaseAt(2u, phase), "tryEventPhaseAt true for end event");
    expectTrue(!fuse::profiler::tryEventPhaseAt(3u, phase), "tryEventPhaseAt false past event count");
void testOrphanAsyncFlowEndGuardPredicates() {
void testChromeTraceExportPreflightGuardFields() {
    expectTrue(emptyPreflight.invalidEventCount == 0u, "preflight invalidEventCount zero on reset");
    expectTrue(!emptyPreflight.hasInvalidEventsInBuffer(),
    expectTrue(!emptyPreflight.hasExportWarnings, "preflight hasExportWarnings false on reset");
    expectTrue(!emptyPreflight.needsFlowNestingCleanup,
    const fuse::profiler::ChromeTraceExportPreflight validPreflight =
    expectTrue(validPreflight.exportableEventCount == 2u,
    expectTrue(validPreflight.invalidEventCount == 0u,
    expectTrue(!validPreflight.hasExportWarnings,
    expectTrue(openPreflight.hasExportWarnings, "preflight warns on open async flow");
    expectTrue(openPreflight.hasOpenAsyncFlows, "preflight open flow flag set with begin only");
    expectTrue(detachedPreflight.needsFlowNestingCleanup,
    expectTrue(detachedPreflight.hasExportWarnings,
    testChromeTraceExportPreflightGuardFields();

// --- deepen additive from deepen-b16-profiler-guards-93c0 ---
void testEventNamePreflightGuard() {
    const fuse::profiler::EventNamePreflight nullPreflight = fuse::profiler::preflightEventName(nullptr);
    expectTrue(nullPreflight.nullName, "preflightEventName marks null name");
    expectTrue(!nullPreflight.emptyName, "preflightEventName does not mark empty for null");
    expectTrue(!nullPreflight.canRecord(), "preflightEventName blocks null name");
    const fuse::profiler::EventNamePreflight emptyPreflight = fuse::profiler::preflightEventName("");
    expectTrue(!emptyPreflight.nullName, "preflightEventName does not mark null for empty string");
    expectTrue(emptyPreflight.emptyName, "preflightEventName marks empty string");
    expectTrue(!emptyPreflight.canRecord(), "preflightEventName blocks empty string");
    const fuse::profiler::EventNamePreflight validPreflight = fuse::profiler::preflightEventName("scope");
    expectTrue(!validPreflight.nullName, "preflightEventName clears null flag for valid name");
    expectTrue(!validPreflight.emptyName, "preflightEventName clears empty flag for valid name");
    expectTrue(validPreflight.canRecord(), "preflightEventName allows valid name");
    const fuse::profiler::EventNamePreflight disabledPreflight = fuse::profiler::preflightEventName("scope");
    expectTrue(disabledPreflight.profilerDisabled, "preflightEventName marks disabled profiler");
    expectTrue(!disabledPreflight.canRecord(), "preflightEventName blocks when profiler disabled");
void testCanRecordEventGuard() {
void testAsyncFlowPreflightGuard() {
    const fuse::profiler::AsyncFlowPreflight nullBegin = fuse::profiler::preflightAsyncFlowBegin(nullptr);
    expectTrue(nullBegin.nullName, "preflightAsyncFlowBegin marks null name");
    expectTrue(!nullBegin.canBegin(), "preflightAsyncFlowBegin blocks null name");
    const fuse::profiler::AsyncFlowPreflight emptyBegin = fuse::profiler::preflightAsyncFlowBegin("");
    expectTrue(emptyBegin.emptyName, "preflightAsyncFlowBegin marks empty string");
    expectTrue(!emptyBegin.canBegin(), "preflightAsyncFlowBegin blocks empty string");
    const fuse::profiler::AsyncFlowPreflight validBegin = fuse::profiler::preflightAsyncFlowBegin("flow");
    expectTrue(validBegin.canBegin(), "preflightAsyncFlowBegin allows valid name");
    const fuse::profiler::AsyncFlowPreflight endWithoutBegin = fuse::profiler::preflightAsyncFlowEnd("flow");
    expectTrue(endWithoutBegin.noOpenFlows, "preflightAsyncFlowEnd marks no open flows");
    expectTrue(!endWithoutBegin.canEnd(), "preflightAsyncFlowEnd blocks without open flow");
    const fuse::profiler::AsyncFlowPreflight endWithBegin = fuse::profiler::preflightAsyncFlowEnd("flow");
    expectTrue(!endWithBegin.noOpenFlows, "preflightAsyncFlowEnd clears noOpenFlows after begin");
    expectTrue(endWithBegin.canEnd(), "preflightAsyncFlowEnd allows end with open flow");
void testCanBeginAndEndAsyncFlowGuard() {
               "tryExportableEventAt true for counter event");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Counter, "tryExportableEventAt copies counter phase");
    expectTrue(!fuse::profiler::tryExportableEventAt(99u, outEvent),
               "tryExportableEventAt false when out of range");
void testCountAndFindEventsByPhaseGuard() {
    const fuse::profiler::ChromeTraceExportPreflight resetPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(!resetPreflight.bufferFull, "preflight bufferFull false on reset");
    expectTrue(!resetPreflight.hasExportWarnings(), "preflight hasExportWarnings false on reset");
        expectTrue(activePreflight.hasExportWarnings(), "preflight warns on active unbalanced scope");
        expectTrue(activePreflight.scopeNestingUnbalanced, "preflight marks scope unbalanced inside scope");
    const fuse::profiler::ChromeTraceExportPreflight openFlowPreflight =
    expectTrue(openFlowPreflight.hasExportWarnings(), "preflight warns on open async flow");
    expectTrue(openFlowPreflight.hasOpenAsyncFlows, "preflight marks open flow for export warning");
    expectTrue(detachedPreflight.hasExportWarnings(), "preflight warns on detached flow depth");
    expectTrue(detachedPreflight.flowDepthDetached, "preflight marks detached flow for export warning");
    testEventNamePreflightGuard();
    testAsyncFlowPreflightGuard();

// --- deepen additive from deepen-fuse-b16-profiler-ddbd ---
    expectTrue(!fuse::profiler::tryExportableEventAt(3u, outEvent),
               "tryFirstExportableEvent copies first begin phase");
               "tryLastExportableEvent copies last end phase");
void testRingWrapAndBufferFullGuards() {
void testChromeTraceExportPreflightCanSafelyExport() {
        expectTrue(activePreflight.canExport(), "preflight canExport inside active scope");
        expectTrue(!activePreflight.canSafelyExport(),
    const fuse::profiler::ChromeTraceExportPreflight balancedPreflight =
    expectTrue(balancedPreflight.canSafelyExport(),
    expectTrue(balancedPreflight.nonExportableEventCount == 0u,
    expectTrue(!balancedPreflight.bufferFull, "preflight bufferFull false with few events");
    expectTrue(!balancedPreflight.hasRingWrapped, "preflight hasRingWrapped false with few events");
    expectTrue(!openFlowPreflight.canSafelyExport(),
    testChromeTraceExportPreflightCanSafelyExport();

// --- deepen additive from deepen-b16-profiler-guards-cad0 ---
void testBlankNameScopeAndFlowGuards() {
void testFindLastEventIndexByPhaseGuard() {
void testLastExportableEventIndexGuard() {
void testTryEventAtPhaseGuard() {
    expectTrue(!fuse::profiler::tryEventAtPhase(0u, fuse::profiler::EventPhase::Begin, outEvent),
               "tryEventAtPhase false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryEventAtPhase clears output on empty buffer");
    expectTrue(fuse::profiler::tryEventAtPhase(0u, fuse::profiler::EventPhase::Begin, outEvent),
               "tryEventAtPhase true for matching begin phase");
               "tryEventAtPhase copies begin event name");
    expectTrue(!fuse::profiler::tryEventAtPhase(0u, fuse::profiler::EventPhase::End, outEvent),
               "tryEventAtPhase false for mismatched phase");
    expectTrue(outEvent.name == nullptr, "tryEventAtPhase clears output on phase mismatch");
    expectTrue(fuse::profiler::tryEventAtPhase(1u, fuse::profiler::EventPhase::End, outEvent),
               "tryEventAtPhase true for matching end phase");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryEventAtPhase copies end phase");
    expectTrue(emptyPreflight.canExportCleanly(), "preflight export cleanly on reset state");
    expectTrue(!emptyPreflight.ringBufferFull, "preflight ringBufferFull false on empty buffer");
    expectTrue(emptyPreflight.nonExportableEventCount == 0u, "preflight nonExportableEventCount zero on reset");
    expectTrue(emptyPreflight.lastExportableEventIndex == fuse::profiler::kInvalidEventIndex,
void testChromeTraceExportPreflightUncleanNesting() {
        const fuse::profiler::ChromeTraceExportPreflight activePreflight = fuse::profiler::preflightChromeTraceExport();
        expectTrue(!activePreflight.canExportCleanly(),
        expectTrue(activePreflight.hasUnbalancedNesting(), "preflight marks unbalanced nesting");
        expectTrue(activePreflight.hasOpenAsyncFlows, "preflight marks open async flows");
    const fuse::profiler::ChromeTraceExportPreflight balancedPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(balancedPreflight.canExportCleanly(), "preflight export cleanly after balanced teardown");
    expectTrue(!openPreflight.canExportCleanly(), "preflight blocks clean export with unmatched flow begin");
    expectTrue(openPreflight.flowNestingUnbalanced, "preflight marks unmatched flow as unbalanced");
void testMixedBlankAndValidNameGuards() {
    testChromeTraceExportPreflightUncleanNesting();

// --- deepen additive from deepen-profiler-b16-guards-33c5 ---
void testWouldRecordEventGuard() {
void testHasActiveScopeAndFlowDepthGuards() {
void testChromeTraceExportPreflightBufferFullAndWarnings() {
    expectTrue(!emptyPreflight.bufferFull, "preflight bufferFull false on reset");
    expectTrue(emptyPreflight.invalidNameEventCount == 0u, "preflight invalidNameEventCount zero on reset");
    expectTrue(!emptyPreflight.canExportWithContent(), "preflight canExportWithContent false on empty buffer");
    expectTrue(emptyPreflight.canExport(), "preflight canExport true when enabled on empty buffer");
    const fuse::profiler::ChromeTraceExportPreflight fullPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(fullPreflight.bufferFull, "preflight marks buffer full");
    expectTrue(fullPreflight.eventCount == fuse::profiler::ringCapacity(),
    expectTrue(fullPreflight.exportableEventCount == fuse::profiler::ringCapacity(),
    expectTrue(fullPreflight.canExportWithContent(), "preflight canExportWithContent true when buffer full");
    expectTrue(!fullPreflight.hasExportWarnings(), "full balanced buffer has no export warnings");
    const fuse::profiler::ChromeTraceExportPreflight warningPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(warningPreflight.hasExportWarnings(), "preflight export warnings with open async flow");
    expectTrue(warningPreflight.hasOpenAsyncFlows, "preflight marks open flows in warning state");
    testChromeTraceExportPreflightBufferFullAndWarnings();

// --- deepen additive from deepen-b16-profiler-guards-3fb8 ---
void testFirstLastEventIndexGuards() {
void testEmptyNameScopeInsideValidScopeGuard() {
void testChromeTraceExportPreflightCapacityAndExportability() {
    expectTrue(emptyPreflight.remainingCapacity == fuse::profiler::ringCapacity(),
    expectTrue(emptyPreflight.allEventsExportable, "preflight allEventsExportable on empty buffer");
    expectTrue(emptyPreflight.readyForExport(), "preflight readyForExport on empty enabled buffer");
    expectTrue(!emptyPreflight.hasOrphanAsyncFlowEnds, "preflight hasOrphanAsyncFlowEnds false on reset");
void testChromeTraceExportPreflightOrphanFlowEnds() {
    testChromeTraceExportPreflightCapacityAndExportability();
    testChromeTraceExportPreflightOrphanFlowEnds();

// --- deepen additive from deepen-fuse-b16-profiler-guards-4db5 ---
void testExportableEventIndexLookupGuard() {
void testTryExportableEventLookupGuard() {
    expectTrue(fuse::profiler::tryFirstExportableEvent(outEvent), "tryFirstExportableEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryFirstExportableEvent copies begin phase");
               "tryFirstExportableEvent copies scope name");
               "tryFirstExportableEvent output passes isExportableProfileEvent");
    expectTrue(fuse::profiler::tryLastExportableEvent(outEvent), "tryLastExportableEvent true after recording");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastExportableEvent copies end phase");
               "tryLastExportableEvent copies scope name");
void testReconcileDetachedFlowNestingGuard() {
void testChromeTraceExportPreflightInvalidNamesAndBufferFull() {
    expectTrue(!disabledPreflight.canExportClean(), "preflight canExportClean false when disabled");
void testIsExportableProfileEventGuard() {
    testChromeTraceExportPreflightInvalidNamesAndBufferFull();

// --- deepen additive from deepen-b16-profiler-guards-9145 ---
void testNullAndEmptyEventNameSplitGuards() {
void testEventNameAtAndTryEventPhaseAtGuards() {
    expectTrue(fuse::profiler::tryEventPhaseAt(1u, outPhase), "tryEventPhaseAt true for counter");
    expectTrue(outPhase == fuse::profiler::EventPhase::Counter, "tryEventPhaseAt copies counter phase");
    expectTrue(fuse::profiler::tryEventPhaseAt(2u, outPhase), "tryEventPhaseAt true for end");
    expectTrue(!fuse::profiler::tryEventPhaseAt(3u, outPhase), "tryEventPhaseAt false past event count");
    expectTrue(fuse::profiler::tryFindLastEventByPhase(fuse::profiler::EventPhase::FlowFinish, outEvent),
               "tryFindLastEventByPhase finds flow finish");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::FlowFinish, "tryFindLastEventByPhase copies phase");
               "tryFindLastEventByPhase copies flow name");
    expectTrue(!fuse::profiler::tryFindLastEventByPhase(
               "tryFindLastEventByPhase false for absent phase");
    expectTrue(outEvent.name == nullptr, "tryFindLastEventByPhase clears output for absent phase");
void testCanEndAsyncFlowAndOrphanGuards() {
    expectTrue(!openPreflight.isExportRecommended(),
    expectTrue(detachedPreflight.flowDepthDetached, "preflight extended marks detached flow depth");
    expectTrue(!detachedPreflight.isExportRecommended(),
    expectTrue(detachedPreflight.nonExportableEventCount == 0u,

// --- deepen additive from deepen-b16-profiler-guards-38ed ---
void testFirstAndLastExportableEventIndexGuard() {
               "tryExportableEventAt true for exportable begin event");
               "tryExportableEventAt true for exportable end event");
void testHasActiveScopeAndAsyncFlowNestingGuard() {
void testChromeTraceExportPreflightExportableIndices() {
    expectTrue(emptyPreflight.firstExportableEventIndex == fuse::profiler::kInvalidEventIndex,
    expectTrue(!emptyPreflight.bufferFull, "preflight bufferFull false on empty buffer");
    testChromeTraceExportPreflightExportableIndices();

// --- deepen additive from deepen-b16-profiler-guards-7722 ---
void testNullOrEmptyEventNameGuard() {
void testWouldRecordWithNameGuard() {
void testTryFindEventByPhaseGuard() {
    expectTrue(!fuse::profiler::tryFindFirstEventByPhase(fuse::profiler::EventPhase::Begin, outEvent),
    expectTrue(!fuse::profiler::tryEventAtPhase(0u, fuse::profiler::EventPhase::Counter, outEvent),
    expectTrue(fuse::profiler::tryFindFirstEventByPhase(fuse::profiler::EventPhase::Begin, outEvent),
               "tryFindFirstEventByPhase true for scope begin");
               "tryFindFirstEventByPhase copies begin scope name");
    expectTrue(fuse::profiler::tryEventAtPhase(1u, fuse::profiler::EventPhase::Counter, outEvent),
               "tryEventAtPhase true when index and phase match");
    expectTrue(outEvent.counterIntValue == 8, "tryEventAtPhase copies counter value");
    expectTrue(!fuse::profiler::tryEventAtPhase(1u, fuse::profiler::EventPhase::Begin, outEvent),
               "tryEventAtPhase false when phase mismatches");
    expectTrue(!fuse::profiler::tryEventAtPhase(9u, fuse::profiler::EventPhase::Counter, outEvent),
               "tryEventAtPhase false when index out of range");
    expectTrue(fuse::profiler::tryFindFirstEventByName("name_inner", outEvent),
               "tryFindFirstEventByName true for recorded name");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryFindFirstEventByName copies phase");
    expectTrue(!fuse::profiler::tryFindFirstEventByName("", outEvent),
               "tryFindFirstEventByName false for empty name");
    expectTrue(outEvent.name == nullptr, "tryFindFirstEventByName clears output for empty name");
void testFlowDepthMismatchGuard() {
void testChromeTraceExportPreflightBufferAndExportReady() {
    expectTrue(!emptyPreflight.isExportReady(), "preflight isExportReady false on empty buffer");
    expectTrue(emptyPreflight.isNestingClean(), "preflight isNestingClean on reset");
    const fuse::profiler::ChromeTraceExportPreflight readyPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(readyPreflight.isExportReady(), "preflight isExportReady with exportable events");
    expectTrue(readyPreflight.isNestingClean(), "preflight isNestingClean after balanced scope");
    expectTrue(!readyPreflight.bufferFull, "preflight bufferFull false after few events");
    expectTrue(readyPreflight.nonExportableEventCount == 0u,
    expectTrue(readyPreflight.exportableEventCount == readyPreflight.eventCount,
    expectTrue(closedPreflight.isNestingClean(), "preflight isNestingClean after balanced teardown");
    testChromeTraceExportPreflightBufferAndExportReady();

// --- deepen additive from deepen-b16-profiler-guards-6796 ---
void testIsEmptyProfileEventGuard() {
void testTryRecordedEventAtGuard() {
    expectTrue(!fuse::profiler::tryRecordedEventAt(0u, outEvent),
               "tryRecordedEventAt false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryRecordedEventAt clears output on empty buffer");
    expectTrue(fuse::profiler::tryRecordedEventAt(0u, outEvent),
               "tryRecordedEventAt true for first recorded event");
               "tryRecordedEventAt copies begin phase without name validation");
               "tryRecordedEventAt copies event name");
    expectTrue(fuse::profiler::tryRecordedEventAt(1u, outEvent),
               "tryRecordedEventAt true for last recorded event");
               "tryRecordedEventAt copies end phase");
    expectTrue(!fuse::profiler::tryRecordedEventAt(2u, outEvent),
               "tryRecordedEventAt false past event count");
    expectTrue(outEvent.name == nullptr, "tryRecordedEventAt clears output when out of range");
               "tryEventAt still succeeds for valid recorded events");
               "tryRecordedEventAt succeeds where tryEventAt succeeds on valid paths");
void testChromeTraceExportPreflightEventIndices() {
    expectTrue(!emptyPreflight.hasValidEventIndices(),
void testChromeTraceExportPreflightBufferFull() {
    expectTrue(!resetPreflight.flowDepthDetached,
    expectTrue(resetPreflight.flowNestingUnbalanced == false,
    testChromeTraceExportPreflightEventIndices();
    testChromeTraceExportPreflightBufferFull();

// --- deepen additive from deepen-b16-profiler-guards-58e6 ---
void testInvalidEventIndexLookupGuards() {
    expectTrue(!fuse::profiler::tryEventAt(fuse::profiler::kInvalidEventIndex, outEvent),
               "tryEventAt false for kInvalidEventIndex on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryEventAt clears output for kInvalidEventIndex");
               "tryEventAt false for kInvalidEventIndex with recorded events");
    expectTrue(fuse::profiler::tryEventAt(0u, outEvent), "tryEventAt succeeds for oldest after wrap");
    expectTrue(outEvent.counterIntValue == 1, "tryEventAt copies oldest counter after wrap");
    expectTrue(fuse::profiler::tryLastEvent(outEvent), "tryLastEvent succeeds after wrap");
               "tryLastEvent copies newest counter after wrap");
void testEmptyNameInsideActiveScopeGuards() {
void testChromeTraceExportPreflightDisabledWithEvents() {
    const fuse::profiler::ChromeTraceExportPreflight enabledPreflight = fuse::profiler::preflightChromeTraceExport();
    expectTrue(enabledPreflight.canExport(), "preflight allows export after re-enable");
    expectTrue(enabledPreflight.hasExportableEvents(), "preflight exportable events survive re-enable");
void testChromeTraceExportPreflightUnbalancedAfterReset() {
    testChromeTraceExportPreflightDisabledWithEvents();
    testChromeTraceExportPreflightUnbalancedAfterReset();

// --- deepen additive from deepen-b16-profiler-guards-61a8 ---
void testCountEventsOfPhaseGuard() {
void testFirstAndLastEventIndexOfPhaseGuard() {
void testTryFirstAndLastEventOfPhaseGuard() {
    expectTrue(!fuse::profiler::tryFirstEventOfPhase(fuse::profiler::EventPhase::Begin, outEvent),
               "tryFirstEventOfPhase false on empty buffer");
    expectTrue(outEvent.name == nullptr, "tryFirstEventOfPhase clears output on empty buffer");
    expectTrue(!fuse::profiler::tryLastEventOfPhase(fuse::profiler::EventPhase::End, outEvent),
               "tryLastEventOfPhase false on empty buffer");
    expectTrue(fuse::profiler::tryFirstEventOfPhase(fuse::profiler::EventPhase::Begin, outEvent),
               "tryFirstEventOfPhase true after recording");
               "tryFirstEventOfPhase copies matching begin event");
    expectTrue(fuse::profiler::tryLastEventOfPhase(fuse::profiler::EventPhase::End, outEvent),
               "tryLastEventOfPhase true after recording");
               "tryLastEventOfPhase copies end phase");
    expectTrue(!fuse::profiler::tryFindEventByName(nullptr, outIndex),
    expectTrue(!fuse::profiler::tryFindEventByName("", outIndex),
    expectTrue(!fuse::profiler::tryFindEventByName("missing", outIndex),
    expectTrue(fuse::profiler::tryFindEventByName("find_me", outIndex),
               "tryFindEventByName finds first matching scope name");
    expectTrue(outIndex == 0u, "tryFindEventByName returns begin index");
               "tryFindEventByName index points at begin event");
    expectTrue(fuse::profiler::tryFindEventByName("find_counter", outIndex),
               "tryFindEventByName finds counter track name");
    expectTrue(outIndex == 1u, "tryFindEventByName returns counter index");
               "tryFindEventByName false for absent name");
void testChromeTraceExportPreflightBufferFullAndCleanExport() {
    expectTrue(openPreflight.canExport(), "preflight still allows export with open flow");
    expectTrue(!openPreflight.canExportCleanly(), "preflight canExportCleanly false with open flow");
    expectTrue(closedPreflight.canExportCleanly(), "preflight canExportCleanly after flow closes");
void testChromeTraceExportPreflightDetachedFlowNotClean() {
    const fuse::profiler::ChromeTraceExportPreflight reconciledPreflight =
    expectTrue(reconciledPreflight.canExportCleanly(),
    testChromeTraceExportPreflightBufferFullAndCleanExport();
    testChromeTraceExportPreflightDetachedFlowNotClean();

// --- deepen additive from deepen-b16-profiler-guards-ce9d ---
    expectTrue(!fuse::profiler::tryFindEventByName(nullptr, outEvent),
               "tryFindEventByName false for null name");
    expectTrue(!fuse::profiler::tryFindEventByName("", outEvent),
               "tryFindEventByName false for empty name");
    expectTrue(!fuse::profiler::tryFindEventByName("missing", outEvent),
    expectTrue(fuse::profiler::tryFindEventByName("find_me", outEvent),
               "tryFindEventByName finds scope begin");
               "tryFindEventByName returns first matching event phase");
               "tryFindEventByName copies matching name");
    expectTrue(fuse::profiler::tryFindEventByName("find_counter", outEvent),
               "tryFindEventByName finds counter by track name");
               "tryFindEventByName finds counter phase");
    expectTrue(outEvent.counterIntValue == 8, "tryFindEventByName copies counter payload");
void testTryFirstEventOfPhaseGuard() {
    expectTrue(fuse::profiler::tryFirstEventOfPhase(fuse::profiler::EventPhase::FlowStart, outEvent),
               "tryFirstEventOfPhase finds first flow start");
               "tryFirstEventOfPhase copies flow start name");
               "tryFirstEventOfPhase finds first scope begin");
               "tryFirstEventOfPhase copies scope begin name");
void testReconcileDetachedFlowNestingDepthGuard() {
    expectTrue(emptyPreflight.canExportClean(), "preflight canExportClean on balanced empty buffer");
    const fuse::profiler::ChromeTraceExportPreflight cleanPreflight =
    expectTrue(cleanPreflight.canExportClean(), "preflight canExportClean with balanced paired scope");
    expectTrue(!cleanPreflight.bufferFull, "preflight bufferFull false below ring capacity");
    expectTrue(!openPreflight.canExportClean(),

// --- deepen additive from deepen-b16-profiler-guards-8f82 ---
    expectTrue(fuse::profiler::hasRejectedInvalidNames(), "hasRejectedInvalidNames after blank attempts");
    expectTrue(!fuse::profiler::tryValidateEventName("   ", reason),
               "tryValidateEventName rejects whitespace-only name");
    expectTrue(reason == fuse::profiler::EventNameRejectReason::Blank, "whitespace maps to Blank reason");
    expectTrue(fuse::profiler::eventNameRejectReason(nullptr) == fuse::profiler::EventNameRejectReason::Null,
    expectTrue(fuse::profiler::eventNameRejectReason("") == fuse::profiler::EventNameRejectReason::Empty,
void testEventLookupRejectReasonGuards() {
    expectTrue(fuse::profiler::eventLookupRejectReason(99u) == fuse::profiler::EventLookupRejectReason::EmptyBuffer,
               "eventLookupRejectReason empty buffer on OOB when empty");
    expectTrue(fuse::profiler::eventLookupRejectReason(2u) == fuse::profiler::EventLookupRejectReason::OutOfRange,
    expectTrue(fuse::profiler::tryExportableEventAt(0u, exportable),
    expectTrue(exportable.phase == fuse::profiler::EventPhase::Begin, "tryExportableEventAt copies begin phase");
    expectTrue(!fuse::profiler::tryExportableEventAt(99u, exportable),
    expectTrue(exportable.name == nullptr, "tryExportableEventAt clears output on failure");
    expectTrue(fuse::profiler::tryFirstExportableEvent(firstExportable), "tryFirstExportableEvent succeeds");
    expectTrue(fuse::profiler::tryLastExportableEvent(lastExportable), "tryLastExportableEvent succeeds");
               "tryFirstExportableEvent returns begin");
               "tryLastExportableEvent returns end");
void testNestingStateRejectReasonGuard() {
    expectTrue(reason == fuse::profiler::NestingStateRejectReason::None, "reset nesting reason is None");
        expectTrue(fuse::profiler::nestingStateRejectReason()
    expectTrue(fuse::profiler::nestingStateRejectReason() == fuse::profiler::NestingStateRejectReason::None,
void testChromeTraceExportRejectReasonGuards() {
    expectTrue(!fuse::profiler::tryExportChromeTraceJson(json, &reason),
               "tryExport sets NoExportableEvents reason");
    expectTrue(json.empty(), "tryExport clears output json on failure");
    expectTrue(fuse::profiler::tryExportChromeTraceJson(json, &reason),
               "tryExportChromeTraceJson succeeds with valid events");
    expectTrue(reason == fuse::profiler::ChromeTraceExportRejectReason::None, "success reason is None");
               "tryExport returns populated trace json");
void testPreflightReportsBufferOverflow() {
    expectTrue(preflight.rejectReason == fuse::profiler::ChromeTraceExportRejectReason::BufferOverflow,
void testRejectReasonLabels() {
                   fuse::profiler::EventNameRejectReason::Blank)) == "blank",
                   fuse::profiler::EventLookupRejectReason::OutOfRange)) == "out_of_range",
                   fuse::profiler::NestingStateRejectReason::OpenAsyncFlows)) == "open_async_flows",
                   fuse::profiler::ChromeTraceExportRejectReason::BufferOverflow)) == "buffer_overflow",
    testEventLookupRejectReasonGuards();
    testNestingStateRejectReasonGuard();
    testChromeTraceExportRejectReasonGuards();
    testPreflightReportsBufferOverflow();
    testRejectReasonLabels();

// --- deepen additive from deepen-fuse-b16-profiler-e55b ---
    expectTrue(!fuse::profiler::tryFindLastEventIndexByName("scope", outIndex),
               "tryFindLastEventIndexByName false on empty buffer");
               "tryFindFirstEventIndexByName false for null name");
               "tryFindFirstEventIndexByName clears output for null name");
    expectTrue(fuse::profiler::tryFindFirstEventIndexByPhase(fuse::profiler::EventPhase::Begin, outIndex),
               "tryFindFirstEventIndexByPhase true after recording");
    expectTrue(outIndex == 0u, "tryFindFirstEventIndexByPhase copies begin index");
    expectTrue(fuse::profiler::tryFindLastEventIndexByPhase(fuse::profiler::EventPhase::End, outIndex),
               "tryFindLastEventIndexByPhase true after recording");
    expectTrue(outIndex == 4u, "tryFindLastEventIndexByPhase copies scope end index");
    expectTrue(fuse::profiler::tryFindFirstEventIndexByName("try_find_flow", outIndex),
               "tryFindFirstEventIndexByName true for flow name");
    expectTrue(outIndex == 1u, "tryFindFirstEventIndexByName copies flow start index");
    expectTrue(fuse::profiler::tryFindLastEventIndexByName("try_find_counter", outIndex),
               "tryFindLastEventIndexByName true for counter track");
    expectTrue(outIndex == 2u, "tryFindLastEventIndexByName copies counter index");
void testEventNameAtAndTryLastExportableEventGuards() {
               "tryLastExportableEvent clears output on empty buffer");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryLastExportableEvent copies last end phase");
void testIsInvalidEventIndexGuard() {
void testBufferPairBalanceGuards() {
void testChromeTraceExportPreflightBufferPairs() {
        expectTrue(activePreflight.scopeBeginCount == 1u, "preflight counts active scope begin");
        expectTrue(activePreflight.scopeEndCount == 0u, "preflight scope end count zero inside scope");
        expectTrue(activePreflight.flowStartCount == 1u, "preflight counts open flow start");
        expectTrue(activePreflight.flowFinishCount == 0u, "preflight flow finish count zero inside flow");
        expectTrue(activePreflight.scopePairImbalancedInBuffer,
        expectTrue(activePreflight.flowPairImbalancedInBuffer,
        expectTrue(activePreflight.hasBufferPairImbalance(),
    expectTrue(closedPreflight.scopeBeginCount == closedPreflight.scopeEndCount,
    expectTrue(closedPreflight.flowStartCount == closedPreflight.flowFinishCount,
    expectTrue(!closedPreflight.scopePairImbalancedInBuffer,
    expectTrue(!closedPreflight.flowPairImbalancedInBuffer,
    testChromeTraceExportPreflightBufferPairs();

// --- deepen additive from deepen-b16-profiler-guards-5b61 ---
void testEventNameRejectReasonGuard() {
    expectTrue(fuse::profiler::eventNameRejectReason("scope")
                   fuse::profiler::EventNameRejectReason::Null)) == "null",
                   fuse::profiler::EventNameRejectReason::Empty)) == "empty",
void testWouldRecordAndCanBeginEndGuards() {
    expectTrue(outEvent.name == nullptr, "tryFirstExportableEvent clears output on empty buffer");
void testEventLookupRejectReasonGuard() {
    expectTrue(fuse::profiler::eventLookupRejectReason(0u)
               "eventLookupRejectReason marks empty buffer");
    expectTrue(fuse::profiler::exportableEventLookupRejectReason(99u)
               "eventLookupRejectReason none for valid begin");
    expectTrue(fuse::profiler::exportableEventLookupRejectReason(1u)
               "eventLookupRejectReason out-of-range past event count");
                   fuse::profiler::EventLookupRejectReason::NotExportable)) == "not_exportable",
void testHasActiveScopeAndFlowNestingGuard() {
void testChromeTraceExportRejectReasonGuard() {
    testEventNameRejectReasonGuard();
    testEventLookupRejectReasonGuard();
    testChromeTraceExportRejectReasonGuard();

// --- deepen additive from deepen-profiler-b16-guards-183a ---
void testIgnoredAsyncFlowEndCountGuard() {
    expectTrue(!fuse::profiler::tryFindEventIndexByName("missing", outIndex),
               "tryFindEventIndexByName false on empty buffer");
               "tryFindEventIndexByName clears output on failure");
    expectTrue(fuse::profiler::tryFindEventIndexByName("name_counter", outIndex),
               "tryFindEventIndexByName true for counter");
    expectTrue(outIndex == 1u, "tryFindEventIndexByName returns counter index");
void testFindEventIndexByScopeIdGuard() {
void testExportableEventBoundaryGuards() {
void testChromeTraceExportPreflightIgnoredAsyncFlowEnds() {
    testChromeTraceExportPreflightIgnoredAsyncFlowEnds();

// --- deepen additive from deepen-b16-profiler-guards-9482 ---
void testPeekEventAtGuard() {
void testChromeTraceExportPreflightRingBufferFull() {
    testChromeTraceExportPreflightRingBufferFull();

// --- deepen additive from deepen-profiler-b16-guards-14e0 ---
void testWhitespaceOnlyEventNameGuard() {
void testTryFindEventByNameAndPhaseGuard() {
               "tryFindFirstEventByName rejects empty query");
               "tryFindFirstEventByName clears output on invalid query");
    expectTrue(!fuse::profiler::tryFindLastEventByPhase(fuse::profiler::EventPhase::End, outEvent),
               "tryFindLastEventByPhase false on empty buffer");
    expectTrue(fuse::profiler::tryFindFirstEventByName("try_lookup_scope", outEvent),
               "tryFindFirstEventByName finds scope begin");
    expectTrue(fuse::profiler::tryFindLastEventByName("try_lookup_scope", outEvent),
               "tryFindLastEventByName finds scope end");
               "tryFindLastEventByName copies end phase");
    expectTrue(fuse::profiler::tryFindFirstEventByPhase(fuse::profiler::EventPhase::FlowStart, outEvent),
               "tryFindFirstEventByPhase finds flow start");
               "tryFindFirstEventByPhase copies flow name");
    expectTrue(fuse::profiler::tryFindLastEventByPhase(fuse::profiler::EventPhase::Counter, outEvent),
               "tryFindLastEventByPhase finds counter sample");
    expectTrue(outEvent.counterIntValue == 11, "tryFindLastEventByPhase copies counter payload");
void testUnpairedBufferEventGuards() {
    expectTrue(openPreflight.hasUnpairedAsyncFlowEvents,
    expectTrue(openPreflight.flowStartEventCount == 2u,
    expectTrue(openPreflight.flowFinishEventCount == 1u,
    expectTrue(openPreflight.hasExportWarnings(), "open flow adds export warnings");
    expectTrue(openPreflight.canExportNonEmpty(), "open flow still allows non-empty export");

// --- deepen additive from deepen-b16-profiler-guards-93b3 ---
void testBlankEventNameGuards() {
void testTryFindEventLookupGuards() {
    expectTrue(!fuse::profiler::tryFindFirstEventByName("scope", outEvent),
               "tryFindFirstEventByName false on empty buffer");
    expectTrue(!fuse::profiler::tryFindLastEventByName("scope", outEvent),
               "tryFind* clears output on empty buffer");
    expectTrue(fuse::profiler::tryFindFirstEventByName("lookup_scope", outEvent),
               "tryFindFirstEventByName locates scope begin");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::Begin, "tryFindFirstEventByName copies begin phase");
    expectTrue(fuse::profiler::tryFindLastEventByName("lookup_scope", outEvent),
               "tryFindLastEventByName locates scope end");
    expectTrue(outEvent.phase == fuse::profiler::EventPhase::End, "tryFindLastEventByName copies end phase");
               "tryFindFirstEventByPhase locates counter");
               "tryFindFirstEventByPhase copies counter name");
               "tryEventAtPhase true for matching phase");
void testExportableEventIndexLookupGuards() {
               "tryFirstExportableEvent copies first exportable begin");
               "tryLastExportableEvent copies last exportable end");
void testHasActiveScopeAndFlowDepthMismatchGuards() {
void testChromeTraceExportPreflightNonExportableCount() {
    expectTrue(emptyPreflight.nonExportableEventCount == emptyPreflight.invalidNameEventCount,
    expectTrue(closedPreflight.nonExportableEventCount == 0u,
    expectTrue(closedPreflight.canExportSafely(), "valid balanced trace can export safely");
    expectTrue(closedPreflight.exportableEventCount + closedPreflight.nonExportableEventCount
    expectTrue(fuse::profiler::tryFindFirstEventByName("valid_track", counterEvent),
               "tryFindFirstEventByName succeeds after blank-name attempts");
    testChromeTraceExportPreflightNonExportableCount();

// --- deepen additive from deepen-b16-profiler-guards-5734 ---
void testScopeBeginEndMismatchGuard() {
void testFlowStartFinishMismatchGuard() {
void testChromeTraceExportPreflightEventPairing() {
    const fuse::profiler::ChromeTraceExportPreflight unmatchedPreflight =
    expectTrue(unmatchedPreflight.flowStartFinishMismatch,
    expectTrue(unmatchedPreflight.hasEventPairingMismatch(),
    expectTrue(!unmatchedPreflight.canExportSafely(),
    testChromeTraceExportPreflightEventPairing();

// --- deepen additive from deepen-b16-profiler-guards-54be ---
void testHasEventsByPhaseGuard() {
               "tryFindFirstEventByPhase locates scope begin");
               "tryFindFirstEventByPhase copies begin phase");
               "tryFindFirstEventByPhase copies begin name");
    expectTrue(fuse::profiler::tryFindLastEventByPhase(fuse::profiler::EventPhase::End, outEvent),
               "tryFindLastEventByPhase locates scope end");
               "tryFindLastEventByPhase copies end phase");
               "tryFindFirstEventByPhase locates counter sample");
    expectTrue(outEvent.counterIntValue == 8, "tryFindFirstEventByPhase copies counter payload");
void testTryLastExportableEventGuard() {
               "tryLastExportableEvent ignores empty-name counter attempts");
               "tryLastExportableEvent still returns last valid end after empty-name attempt");
void testReconcilePendingFlowHandoffGuard() {

// --- deepen additive from deepen-b16-profiler-guards-a835 ---
void testFirstLastExportableEventIndexGuard() {
void testTryFirstLastExportableEventGuard() {
               "tryFirstExportableEvent true for begin event");
               "tryLastExportableEvent true for end event");
void testHasActiveProfilingNestingGuard() {
void testChromeTraceExportPreflightCleanExportFlags() {
    expectTrue(emptyPreflight.canExportCleanly(), "empty balanced buffer can export cleanly");
    expectTrue(!emptyPreflight.hasActiveProfilingNesting,
    expectTrue(emptyPreflight.droppedEventCount == 0u, "empty preflight dropped count is zero");
        expectTrue(activePreflight.hasActiveProfilingNesting,
    expectTrue(closedPreflight.canExportCleanly(), "balanced trace can export cleanly");
    const fuse::profiler::ChromeTraceExportPreflight overflowPreflight =
    expectTrue(overflowPreflight.ringBufferFull, "overflow preflight marks ring buffer full");
    expectTrue(overflowPreflight.droppedEventCount == 1u, "overflow preflight reports dropped events");
    expectTrue(!overflowPreflight.canExportCleanly(), "overflow preflight blocks clean export");
    testChromeTraceExportPreflightCleanExportFlags();

// --- deepen additive from deepen-b16-profiler-guards-a136 ---
void testBlankNameGuards() {
               "tryFirstExportableEvent true for first exportable event");
               "tryLastExportableEvent true for last exportable event");
void testNestedAsyncFlowContextGuards() {

// --- deepen additive from b16-profiler-deepen-guards-5051 ---
    expectTrue(!fuse::profiler::tryFindFirstEventIndexByName("missing", outIndex),
               "tryFindFirstEventIndexByName false on empty buffer");
               "tryFindFirstEventIndexByName clears output on empty buffer");
    expectTrue(fuse::profiler::tryFindFirstEventIndexByName("name_inner", outIndex),
               "tryFindFirstEventIndexByName true for inner scope");
    expectTrue(outIndex == 2u, "tryFindFirstEventIndexByName copies inner begin index");
void testTryFindEventIndexByPhaseGuard() {
    expectTrue(!fuse::profiler::tryFindLastEventIndexByPhase(fuse::profiler::EventPhase::End, outIndex),
               "tryFindLastEventIndexByPhase false on empty buffer");
    expectTrue(fuse::profiler::tryFindFirstEventIndexByPhase(fuse::profiler::EventPhase::FlowStart, outIndex),
               "tryFindFirstEventIndexByPhase locates flow start");
    expectTrue(outIndex == 1u, "tryFindFirstEventIndexByPhase copies flow start index");
    expectTrue(fuse::profiler::tryFindLastEventIndexByPhase(fuse::profiler::EventPhase::Counter, outIndex),
               "tryFindLastEventIndexByPhase locates counter sample");
    expectTrue(outIndex == 2u, "tryFindLastEventIndexByPhase copies counter index");
void testChromeTraceExportPreflightTrimAndOrphanFlags() {
    expectTrue(!emptyPreflight.exportWouldTrimEvents, "empty preflight does not trim events");
    expectTrue(!emptyPreflight.hasOnlyExportableEvents, "empty preflight hasOnlyExportableEvents false");
    expectTrue(emptyPreflight.orphanAsyncFlowEndCount == 0u, "empty preflight orphan count is zero");
    expectTrue(!emptyPreflight.hasOrphanAsyncFlowEnds, "empty preflight hasOrphanAsyncFlowEnds false");
    expectTrue(!closedPreflight.exportWouldTrimEvents, "valid trace does not trim events");
    expectTrue(closedPreflight.hasOnlyExportableEvents, "valid trace hasOnlyExportableEvents true");
    const fuse::profiler::ChromeTraceExportPreflight orphanPreflight =
    expectTrue(orphanPreflight.orphanAsyncFlowEndCount == 1u, "preflight reports orphan flow end count");
    expectTrue(orphanPreflight.hasOrphanAsyncFlowEnds, "preflight marks orphan async flow ends");
    expectTrue(orphanPreflight.canExportSafely(), "orphan ends alone do not block safe export");
    expectTrue(fuse::profiler::tryFindFirstEventIndexByName("lookup_scope", scopeIndex),
               "tryFindFirstEventIndexByName succeeds after blank-name attempts");
    expectTrue(fuse::profiler::tryExportableEventAt(scopeIndex, scopeEvent),
    testChromeTraceExportPreflightTrimAndOrphanFlags();

// --- deepen additive from deepen-b16-profiler-guards-512e ---
void testHasActiveScopesAndFlowNestingConsistencyGuards() {
    expectTrue(emptyPreflight.scopeBeginEventCount == 0u,
    expectTrue(emptyPreflight.counterEventCount == 0u,
    expectTrue(emptyPreflight.flowStartEventCount == 0u,
    expectTrue(!emptyPreflight.hasActiveScopes, "empty preflight has no active scopes");
        expectTrue(activePreflight.hasActiveScopes, "active preflight marks active scopes");
        expectTrue(activePreflight.scopeBeginEventCount == 1u,
        expectTrue(activePreflight.counterEventCount == 1u,
        expectTrue(activePreflight.flowStartEventCount == 1u,
    expectTrue(!closedPreflight.hasActiveScopes, "closed preflight clears active scopes");
    expectTrue(closedPreflight.scopeBeginEventCount == 1u,
    expectTrue(closedPreflight.counterEventCount == 1u,
    expectTrue(closedPreflight.flowStartEventCount == 1u,
    expectTrue(fuse::profiler::tryFirstExportableEvent(beginEvent),
               "tryFirstExportableEvent succeeds after empty-name attempts");
               "tryFirstExportableEvent copies valid scope after empty-name attempts");

// --- deepen additive from deepen-profiler-b16-guards-eb78 ---
               "tryLastEventByName clears output for empty name");
void testTryFlowLookupByIdGuard() {
    expectTrue(!fuse::profiler::tryFirstFlowStartById(0u, outEvent),
               "tryFirstFlowStartById false for zero flow id");
    expectTrue(!fuse::profiler::tryLastFlowFinishById(0u, outEvent),
               "tryLastFlowFinishById false for zero flow id");
    expectTrue(fuse::profiler::tryFirstFlowStartById(flowId, outEvent),
               "tryFirstFlowStartById true for recorded flow start");
               "tryFirstFlowStartById copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFirstFlowStartById copies flow id");
    expectTrue(fuse::profiler::tryLastFlowFinishById(flowId, outEvent),
               "tryLastFlowFinishById true for recorded flow finish");
               "tryLastFlowFinishById copies flow finish phase");
    expectTrue(outEvent.scopeId == flowId, "tryLastFlowFinishById copies flow id");
void testFlowPairingConsistencyGuards() {
void testNestingStateConsistencyGuard() {
void testChromeTraceExportPreflightUnpairedFlows() {
    expectTrue(!closedPreflight.hasUnpairedFlowEvents,
    expectTrue(closedPreflight.isNestingStateConsistent(),
    expectTrue(fuse::profiler::tryFirstEventByName("valid_lookup_counter", counterEvent),
               "tryFirstEventByName succeeds after empty-name attempts");
    testChromeTraceExportPreflightUnpairedFlows();

// --- deepen additive from deepen-b16-profiler-name-flow-guards-4e15 ---
void testEventNameLookupGuards() {
    expectTrue(fuse::profiler::tryFirstEventByName("lookup_scope", namedEvent),
               "tryFirstEventByName succeeds for recorded scope");
    expectTrue(fuse::profiler::tryLastEventByName("lookup_scope", namedEvent),
               "tryLastEventByName succeeds for recorded scope");
    expectTrue(!fuse::profiler::tryFirstEventByName("", namedEvent),
               "tryFirstEventByName rejects empty name");
void testFlowIdLookupGuards() {
    expectTrue(fuse::profiler::tryFirstFlowEventById(flowId, flowEvent),
               "tryFirstFlowEventById succeeds for flow start");
               "tryFirstFlowEventById copies flow start phase");
    expectTrue(fuse::profiler::tryLastFlowEventById(flowId, flowEvent),
               "tryLastFlowEventById succeeds for flow finish");
               "tryLastFlowEventById copies flow finish phase");
void testUnbalancedFlowPairLookupGuards() {
    const fuse::profiler::FlowIdLookupPreflight openPreflight =
        fuse::profiler::preflightFlowLookupById(flowId);
    expectTrue(openPreflight.canLookup(), "flow lookup preflight ok with open flow");
    expectTrue(openPreflight.hasFlowEvents(), "flow lookup preflight sees flow start");
    expectTrue(!openPreflight.isPairBalanced(), "flow lookup preflight marks unbalanced pair");
    expectTrue(openPreflight.flowStartCount == 1u, "flow lookup preflight counts flow start");
    expectTrue(openPreflight.flowFinishCount == 0u, "flow lookup preflight counts zero finishes");
    const fuse::profiler::ChromeTraceExportPreflight exportPreflight =
    expectTrue(exportPreflight.hasUnbalancedFlowPairsInBuffer,
    expectTrue(exportPreflight.unbalancedFlowPairCount == 1u,
    expectTrue(!exportPreflight.canExportSafely(),
void testEventNameLookupPreflight() {
    const fuse::profiler::EventNameLookupPreflight emptyNamePreflight =
        fuse::profiler::preflightEventLookupByName("");
    expectTrue(!emptyNamePreflight.nameValid, "name lookup preflight rejects empty name");
    expectTrue(!emptyNamePreflight.canLookup(), "empty name cannot lookup");
    const fuse::profiler::EventNameLookupPreflight emptyBufferPreflight =
        fuse::profiler::preflightEventLookupByName("missing");
    expectTrue(emptyBufferPreflight.nameValid, "valid name on empty buffer passes nameValid");
    expectTrue(!emptyBufferPreflight.canLookup(), "empty buffer cannot lookup by name");
    expectTrue(!emptyBufferPreflight.hasMatches(), "empty buffer has no name matches");
    const fuse::profiler::EventNameLookupPreflight matchPreflight =
        fuse::profiler::preflightEventLookupByName("preflight_track");
    expectTrue(matchPreflight.canLookup(), "name lookup preflight ok with recorded event");
    expectTrue(matchPreflight.hasMatches(), "name lookup preflight finds counter track");
    expectTrue(matchPreflight.matchCount == 1u, "name lookup preflight counts one match");
    expectTrue(matchPreflight.firstMatchIndex == 0u, "name lookup preflight first index is zero");
    expectTrue(matchPreflight.lastMatchIndex == 0u, "name lookup preflight last index is zero");
void testNestingConsistencyPreflight() {
    const fuse::profiler::NestingConsistencyPreflight resetPreflight =
        fuse::profiler::preflightNestingConsistency();
    expectTrue(resetPreflight.isConsistent(), "reset nesting consistency is balanced");
    expectTrue(resetPreflight.scopeNestingBalanced, "reset scope nesting balanced");
    expectTrue(resetPreflight.flowNestingBalanced, "reset flow nesting balanced");
    expectTrue(resetPreflight.flowDepthAttached, "reset flow depth attached");
        const fuse::profiler::NestingConsistencyPreflight activePreflight =
        expectTrue(!activePreflight.isConsistent(), "open scope and flow report inconsistent nesting");
        expectTrue(!activePreflight.scopeNestingBalanced, "active scope is unbalanced");
        expectTrue(!activePreflight.flowNestingBalanced, "open flow is unbalanced");
        expectTrue(activePreflight.openAsyncFlowCount == 1u, "consistency preflight tracks open flow count");
    const fuse::profiler::NestingConsistencyPreflight closedPreflight =
    expectTrue(closedPreflight.isConsistent(), "closed scope and flow restore consistency");
void testEventMatchesNameAndFlowGuards() {
    testEventNameLookupPreflight();
    testNestingConsistencyPreflight();

// --- deepen additive from deepen-b16-profiler-name-flow-e105 ---
               "tryFirstEventByName clears output for invalid name");
void testTryEventByFlowIdGuard() {
    expectTrue(fuse::profiler::tryFirstEventByFlowId(flowId, outEvent),
               "tryFirstEventByFlowId true for flow start");
               "tryFirstEventByFlowId copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFirstEventByFlowId copies flow id");
    expectTrue(fuse::profiler::tryLastEventByFlowId(flowId, outEvent),
               "tryLastEventByFlowId true for flow finish");
               "tryLastEventByFlowId copies flow finish phase");
    expectTrue(!fuse::profiler::tryFirstEventByFlowId(flowId + 999u, outEvent),
               "tryFirstEventByFlowId false for missing flow id");
               "tryFirstEventByFlowId clears output for missing flow id");
    expectTrue(!emptyPreflight.hasActiveScope, "empty preflight hasActiveScope false");
    expectTrue(!emptyPreflight.hasActiveAsyncFlowNesting,
        expectTrue(activePreflight.firstExportableEventIndex == 0u,
        expectTrue(activePreflight.lastExportableEventIndex == 0u,
        expectTrue(activePreflight.canExportNonEmptyTrace(),
        expectTrue(activePreflight.hasActiveScope, "active preflight marks hasActiveScope");
    expectTrue(closedPreflight.firstExportableEventIndex == 0u,
    expectTrue(closedPreflight.lastExportableEventIndex == 1u,
    expectTrue(closedPreflight.canExportNonEmptyTrace(),
    expectTrue(!closedPreflight.hasActiveScope, "closed preflight clears hasActiveScope");
    expectTrue(fuse::profiler::tryFirstEventByName("lookup_valid_scope", outEvent),

// --- deepen additive from deepen-b16-profiler-name-flow-lookup-cb6d ---
    expectTrue(fuse::profiler::tryFindFirstEventByName("try_name_scope", outEvent),
               "tryFindFirstEventByName true for recorded scope");
    expectTrue(fuse::profiler::tryFindLastEventByName("try_name_scope", outEvent),
               "tryFindLastEventByName true for recorded scope");
void testTryFindEventByFlowIdGuard() {
    expectTrue(fuse::profiler::tryFindFirstEventByFlowId(flowId, outEvent),
               "tryFindFirstEventByFlowId true for flow start");
               "tryFindFirstEventByFlowId copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFindFirstEventByFlowId preserves flow id");
    expectTrue(fuse::profiler::tryFindLastEventByFlowId(flowId, outEvent),
               "tryFindLastEventByFlowId true for flow finish");
               "tryFindLastEventByFlowId copies flow finish phase");
void testBufferedFlowPairConsistencyGuard() {
void testChromeTraceExportPreflightCleanExport() {
    testChromeTraceExportPreflightCleanExport();

// --- deepen additive from deepen-fuse-b16-profiler-guards-9bc9 ---
               "tryFindFirstEventByName false for null name");
               "tryFindFirstEventByName clears output for null name");
    expectTrue(!fuse::profiler::tryFindLastEventByName("", outEvent),
               "tryFindLastEventByName false for empty name");
               "tryFindFirstEventByName copies scope name");
               "tryFindLastEventByName true for scope end");
    expectTrue(!fuse::profiler::tryFindFirstEventByFlowId(0u, outEvent),
               "tryFindFirstEventByFlowId false for zero flow id");
               "tryFindFirstEventByFlowId clears output for zero flow id");
    expectTrue(outEvent.scopeId == flowId, "tryFindFirstEventByFlowId copies flow id");
void testChromeTraceExportPreflightEventPairCounts() {
    expectTrue(emptyPreflight.scopeBeginEventCount == 0u, "empty preflight scope begin count is zero");
    expectTrue(emptyPreflight.scopeEndEventCount == 0u, "empty preflight scope end count is zero");
    expectTrue(emptyPreflight.flowStartEventCount == 0u, "empty preflight flow start count is zero");
    expectTrue(emptyPreflight.flowFinishEventCount == 0u, "empty preflight flow finish count is zero");
    expectTrue(emptyPreflight.counterEventCount == 0u, "empty preflight counter count is zero");
    expectTrue(emptyPreflight.hasPairedScopeEventsInBuffer(), "empty buffer has paired scope events");
    expectTrue(emptyPreflight.hasPairedFlowEventsInBuffer(), "empty buffer has paired flow events");
    expectTrue(emptyPreflight.hasConsistentEventPairsInBuffer(), "empty buffer event pairs are consistent");
    expectTrue(emptyPreflight.canExportSafelyWithConsistentBuffer(),
    expectTrue(!openPreflight.hasPairedFlowEventsInBuffer(),
    expectTrue(!openPreflight.hasConsistentEventPairsInBuffer(),
    expectTrue(!openPreflight.canExportSafelyWithConsistentBuffer(),
    expectTrue(!openPreflight.canExportSafely(),
    testChromeTraceExportPreflightEventPairCounts();

// --- deepen additive from b16-profiler-deepen-guards-8e14 ---
void testTryFindEventByNameAndFlowIdGuard() {
               "tryFindFirstEventIndexByName clears index on miss");
    expectTrue(!fuse::profiler::tryFindFirstEventByName("missing", outEvent),
               "tryFindFirstEventByName clears output on miss");
    expectTrue(!fuse::profiler::tryFindFirstEventIndexByFlowId(7u, outIndex),
               "tryFindFirstEventIndexByFlowId false on empty buffer");
    expectTrue(!fuse::profiler::tryFindFirstEventByFlowId(7u, outEvent),
               "tryFindFirstEventByFlowId false on empty buffer");
    expectTrue(fuse::profiler::tryFindFirstEventIndexByName("try_counter", outIndex),
    expectTrue(outIndex == 1u, "tryFindFirstEventIndexByName returns counter index");
    expectTrue(fuse::profiler::tryFindFirstEventByName("try_flow", outEvent),
               "tryFindFirstEventByName true for flow start");
               "tryFindFirstEventByName copies flow start phase");
    expectTrue(fuse::profiler::tryFindLastEventByName("try_flow", outEvent),
               "tryFindLastEventByName true for flow finish");
               "tryFindLastEventByName copies flow finish phase");
void testTryExportableFirstLastEventGuard() {
               "tryExportableFirstEvent copies begin phase");
               "tryExportableFirstEvent copies scope name");
               "tryExportableLastEvent copies end phase");
    const fuse::profiler::ProfileScopePreflight enabledPreflight =
        fuse::profiler::preflightProfileScope("preflight_scope");
    expectTrue(enabledPreflight.canEnter, "preflight allows valid scope name");
    expectTrue(!enabledPreflight.invalidName, "preflight valid scope name is not invalid");
    expectTrue(!enabledPreflight.profilerDisabled, "preflight profiler enabled on reset");
    expectTrue(!nullPreflight.canEnter, "preflight blocks null scope name");
    expectTrue(nullPreflight.invalidName, "preflight marks null scope name invalid");
    expectTrue(!disabledPreflight.canEnter, "preflight blocks scope when profiler disabled");
    expectTrue(disabledPreflight.profilerDisabled, "preflight marks profiler disabled");
void testPreflightBeginEndAsyncFlowGuard() {
        fuse::profiler::preflightBeginAsyncFlow("preflight_flow", flowId);
    expectTrue(beginPreflight.canBegin, "preflight allows valid flow begin");
    expectTrue(!beginPreflight.invalidName, "preflight valid flow name is not invalid");
    const fuse::profiler::AsyncFlowEndPreflight orphanEndPreflight =
        fuse::profiler::preflightEndAsyncFlow("preflight_flow", flowId);
    expectTrue(!orphanEndPreflight.canEnd, "preflight blocks orphan flow end");
    expectTrue(orphanEndPreflight.wouldUnderflowOpenCount,
    const fuse::profiler::AsyncFlowEndPreflight matchedEndPreflight =
    expectTrue(matchedEndPreflight.canEnd, "preflight allows matched flow end");
    expectTrue(!matchedEndPreflight.wouldUnderflowOpenCount,
void testChromeTraceExportPreflightNameAndOrphanFlags() {
    expectTrue(orphanPreflight.canExportWithEvents(),
    testPreflightBeginEndAsyncFlowGuard();
    testChromeTraceExportPreflightNameAndOrphanFlags();

// --- deepen additive from deepen-b16-profiler-guards-590c ---
void testWouldSkipNameAndFlowLookupGuards() {
    expectTrue(fuse::profiler::wouldSkipNameLookup(nullptr), "null name skips lookup");
    expectTrue(fuse::profiler::wouldSkipNameLookup(""), "empty name skips lookup");
    expectTrue(fuse::profiler::wouldSkipNameLookup("missing"), "missing name skips lookup on empty buffer");
    expectTrue(fuse::profiler::wouldSkipFlowIdLookup(0u), "zero flow id skips lookup");
    expectTrue(fuse::profiler::wouldSkipFlowIdLookup(42u), "non-zero flow id skips lookup on empty buffer");
    expectTrue(!fuse::profiler::wouldSkipNameLookup("lookup_scope"),
    expectTrue(!fuse::profiler::wouldSkipNameLookup("missing"),
    expectTrue(!fuse::profiler::wouldSkipFlowIdLookup(flowId),
void testTryEventLookupByNameAndFlowGuards() {
    expectTrue(!fuse::profiler::tryFirstEventByName("", outEvent),
               "tryFirstEventByName false for empty name");
    expectTrue(outEvent.name == nullptr, "tryFirstEventByName clears output for empty name");
    expectTrue(!fuse::profiler::tryLastFlowEvent(0u, outEvent),
               "tryLastFlowEvent false for zero flow id");
    expectTrue(fuse::profiler::tryFirstEventByName("try_lookup_scope", outEvent),
               "tryFirstEventByName succeeds for scope begin");
               "tryFirstEventByName copies scope begin phase");
    expectTrue(fuse::profiler::tryLastEventByName("try_lookup_counter", outEvent),
               "tryLastEventByName succeeds for counter sample");
               "tryLastEventByName copies counter phase");
               "tryFirstFlowEvent succeeds for flow start");
               "tryLastFlowEvent succeeds for flow finish");
void testRecordedNestingConsistencyGuards() {
void testChromeTraceExportPreflightRecordedNesting() {
    const fuse::u32 preflightBeginFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 preflightOrphanFlowId = fuse::profiler::nextFlowId();
    fuse::profiler::beginAsyncFlow("preflight_mismatched_begin", preflightBeginFlowId);
    fuse::profiler::endAsyncFlow("preflight_mismatched_orphan", preflightOrphanFlowId);
    expectTrue(!orphanPreflight.recordedFlowPairingConsistent,
    expectTrue(orphanPreflight.hasInconsistentRecordedNesting(),
    expectTrue(orphanPreflight.orphanFlowEndCount == 1u,
    expectTrue(!orphanPreflight.canExportSafely(),
    expectTrue(fuse::profiler::tryFirstEventByName("valid_name_lookup", counterEvent),
    testChromeTraceExportPreflightRecordedNesting();

// --- deepen additive from deepen-b16-profiler-name-flow-guards-7257 ---
void testTryFindFirstEventByNameGuard() {
    expectTrue(outEvent.name == nullptr, "tryFindFirstEventByName clears output for null name");
    expectTrue(fuse::profiler::tryFindFirstEventByName("named_counter", outEvent),
               "tryFindFirstEventByName true for recorded counter");
               "tryFindFirstEventByName copies counter phase");
    expectTrue(outEvent.counterIntValue == 8, "tryFindFirstEventByName copies counter value");
void testTryFindFirstEventByFlowIdGuard() {
               "tryFindFirstEventByFlowId true for open flow start");
               "tryFindFirstEventByFlowId copies flow name");
               "tryFindFirstEventByFlowId still finds flow after finish");
               "tryFindFirstEventByFlowId keeps first flow event after finish");
void testFlowIdOpenAndUnpairedGuards() {
    expectTrue(!emptyPreflight.hasUnpairedAsyncFlowEvents,
    expectTrue(emptyPreflight.unpairedAsyncFlowIdCount == 0u,
    expectTrue(!emptyPreflight.hasFlowEventImbalances(),
    expectTrue(openPreflight.unpairedAsyncFlowIdCount == 1u,
    expectTrue(openPreflight.hasFlowEventImbalances(),
    expectTrue(openPreflight.hasOpenAsyncFlows, "preflight still tracks open async flows");
    expectTrue(!closedPreflight.hasUnpairedAsyncFlowEvents,
    expectTrue(closedPreflight.unpairedAsyncFlowIdCount == 0u,
    expectTrue(!closedPreflight.hasFlowEventImbalances(),

// --- deepen additive from b16-profiler-deepen-guards-891a ---
void testFindEventIndexByFlowGuard() {
void testTryFindEventByFlowGuard() {
    expectTrue(!fuse::profiler::tryFindFirstEventByFlow(0u, outEvent),
               "tryFindFirstEventByFlow false for zero flow id");
               "tryFindFirstEventByFlow clears output for zero flow id");
    expectTrue(fuse::profiler::tryFindFirstEventByFlow(flowId, outEvent),
               "tryFindFirstEventByFlow true for open flow start");
               "tryFindFirstEventByFlow copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFindFirstEventByFlow copies flow id");
    expectTrue(fuse::profiler::tryFindLastEventByFlow(flowId, outEvent),
               "tryFindLastEventByFlow true for flow finish");
               "tryFindLastEventByFlow copies flow finish phase");
void testFlowIdBalanceAndOrphanEndGuards() {

// --- deepen additive from deepen-b16-profiler-name-flow-lookup-cedf ---
void testTryFindFirstEventByNameAndFlowIdGuard() {
    expectTrue(!fuse::profiler::tryFindFirstEventByFlowId(42u, outEvent),
               "tryFindFirstEventByName copies scope begin phase");
void testIsAsyncFlowIdPairedGuard() {
void testIsScopeNameBalancedInBufferGuard() {
void testUnpairedAsyncFlowsInBufferGuard() {
    expectTrue(!closedPreflight.hasUnpairedAsyncFlowsInBuffer,
void testHasExportBlockersGuard() {
    expectTrue(!emptyPreflight.hasExportBlockers(),
        expectTrue(activePreflight.hasExportBlockers(),

// --- deepen additive from deepen-b16-profiler-guards-c4cf ---
void testTryEventLookupByNameAndFlowIdGuard() {
    expectTrue(!fuse::profiler::tryFirstEventByFlowId(0u, outEvent),
               "tryFirstEventByFlowId false for zero flow id");
    expectTrue(!fuse::profiler::tryLastEventByFlowId(0u, outEvent),
               "tryLastEventByFlowId false for zero flow id");
    expectTrue(fuse::profiler::tryLastEventByName("try_lookup_scope", outEvent),
               "tryLastEventByName copies scope end phase");
    expectTrue(outEvent.scopeId == flowId, "tryFirstEventByFlowId preserves flow id");
    expectTrue(fuse::profiler::tryFirstEventByName("try_lookup_counter", outEvent),
    expectTrue(!fuse::profiler::tryFirstEventByName("missing_try_name", outEvent),
void testNestingAsyncFlowPreflightGuard() {
    expectTrue(!resetPreflight.hasOpenAsyncFlows, "reset has no open async flows");
    expectTrue(!resetPreflight.flowDepthDetached, "reset flow depth is attached");
    expectTrue(!resetPreflight.crossThreadFlowHandoffPending,
        const fuse::profiler::NestingAsyncFlowPreflight activeScopePreflight =
        expectTrue(!activeScopePreflight.isBalanced(), "active scope reports unbalanced nesting preflight");
        expectTrue(!activeScopePreflight.scopeNestingBalanced,
        expectTrue(activeScopePreflight.activeScopeNestingDepth == 1u,
        expectTrue(activeScopePreflight.maxScopeNestingDepth == 1u,
            const fuse::profiler::NestingAsyncFlowPreflight nestedPreflight =
            expectTrue(!nestedPreflight.isBalanced(), "nested scope/flow preflight is unbalanced");
            expectTrue(nestedPreflight.activeScopeNestingDepth == 2u,
            expectTrue(nestedPreflight.activeFlowNestingDepth == 2u,
            expectTrue(nestedPreflight.openAsyncFlowCount == 2u,
            expectTrue(nestedPreflight.hasOpenAsyncFlows, "nested preflight marks open async flows");
            expectTrue(nestedPreflight.maxFlowNestingDepth == 2u,
    expectTrue(closedPreflight.isBalanced(), "closed nesting preflight is balanced");
    expectTrue(!closedPreflight.hasOpenAsyncFlows, "closed nesting preflight clears open flows");
    const fuse::profiler::NestingAsyncFlowPreflight handoffPreflight =
    expectTrue(!handoffPreflight.isBalanced(), "cross-thread handoff preflight is unbalanced");
    expectTrue(handoffPreflight.flowDepthDetached, "handoff preflight marks detached flow depth");
    expectTrue(handoffPreflight.crossThreadFlowHandoffPending,
    expectTrue(!handoffPreflight.hasOpenAsyncFlows, "handoff preflight clears global open flow count");
    testNestingAsyncFlowPreflightGuard();

// --- deepen additive from deepen-fuse-b16-profiler-b63b ---
    expectTrue(fuse::profiler::tryFirstEventByName("lookup_inner", outEvent),
               "tryFirstEventByName succeeds for inner scope");
               "tryFirstEventByName copies inner begin phase");
    expectTrue(fuse::profiler::tryLastEventByName("lookup_inner", outEvent),
               "tryLastEventByName succeeds for inner scope");
               "tryLastEventByName copies inner end phase");
               "tryFirstEventByFlowId succeeds for flow start");
               "tryLastEventByFlowId succeeds for flow finish");

// --- deepen additive from deepen-b16-profiler-name-flow-lookup-c5ea ---
void testHasActiveScopeAndFlowNestingGuards() {
    expectTrue(fuse::profiler::tryFirstEventByName("lookup_counter", outEvent),
               "tryFirstEventByName true for first counter sample");
    expectTrue(outEvent.counterIntValue == 3, "tryFirstEventByName copies first counter value");
    expectTrue(fuse::profiler::tryLastEventByName("lookup_counter", outEvent),
               "tryLastEventByName true for last counter sample");
    expectTrue(outEvent.counterIntValue == 5, "tryLastEventByName copies last counter value");
               "tryFirstEventByName clears output on empty name");
    expectTrue(fuse::profiler::tryFirstFlowEvent(innerFlowId, outEvent),
               "tryFirstFlowEvent true for inner flow start");
    expectTrue(fuse::profiler::tryLastFlowEvent(outerFlowId, outEvent),
               "tryLastFlowEvent true for outer flow finish");
    expectTrue(outEvent.scopeId == outerFlowId, "tryLastFlowEvent preserves flow id");

// --- deepen additive from profiler-deepen-guards-fd14 ---
    expectTrue(!fuse::profiler::tryFindLastEventByFlowId(flowId, outEvent),
               "tryFindLastEventByFlowId false on empty buffer");

// --- deepen additive from profiler-name-flow-lookup-preflight-e5cc ---
void testUnpairedFlowLookupAndPreflight() {
    expectTrue(!closedPreflight.hasUnpairedFlowEvents, "preflight clears unpaired flow flag after end");
    expectTrue(closedPreflight.flowStartEventCount == 1u, "preflight keeps flow start count after end");
    expectTrue(closedPreflight.flowFinishEventCount == 1u, "preflight counts flow finish after end");
    expectTrue(closedPreflight.canExportSafely(), "paired flow can export safely");
    testUnpairedFlowLookupAndPreflight();

// --- deepen additive from deepen-b16-profiler-guards-35d2 ---
    expectTrue(outEvent.name == nullptr, "tryFindFirstEventByName clears output on miss");
    expectTrue(!fuse::profiler::tryFindFirstEventByFlowId(flowId, outEvent),
    expectTrue(fuse::profiler::tryFindFirstEventByName("try_counter", outEvent),
               "tryFindFirstEventByName true for counter track");
               "tryFindFirstEventByName rejects null lookup name");
               "tryFindFirstEventByName rejects empty lookup name");
               "tryExportableLastEvent still returns last exportable event");
               "tryExportableLastEvent skips invalid-name tail events");
        fuse::profiler::preflightProfileScope("scope_probe");
    expectTrue(!disabledPreflight.canEnter, "preflightProfileScope blocks when profiler disabled");
    expectTrue(disabledPreflight.profilerDisabled, "preflightProfileScope marks profiler disabled");
    const fuse::profiler::ProfileScopePreflight nullPreflight = fuse::profiler::preflightProfileScope(nullptr);
    expectTrue(!nullPreflight.canEnter, "preflightProfileScope blocks null name");
    expectTrue(nullPreflight.invalidName, "preflightProfileScope marks null name invalid");
    const fuse::profiler::ProfileScopePreflight emptyPreflight = fuse::profiler::preflightProfileScope("");
    expectTrue(!emptyPreflight.canEnter, "preflightProfileScope blocks empty name");
    expectTrue(emptyPreflight.invalidName, "preflightProfileScope marks empty name invalid");
    expectTrue(validPreflight.canEnter, "preflightProfileScope allows valid scope name");
    expectTrue(!validPreflight.invalidName, "preflightProfileScope clears invalid flag for valid name");
    expectTrue(!nullBegin.canBegin, "preflightBeginAsyncFlow blocks null name");
    expectTrue(nullBegin.invalidName, "preflightBeginAsyncFlow marks null name invalid");
    expectTrue(!orphanEnd.canEnd, "preflightEndAsyncFlow blocks orphan finish");
               "preflightEndAsyncFlow marks orphan finish as open-count underflow");
    expectTrue(validBegin.canBegin, "preflightBeginAsyncFlow allows valid begin");
    expectTrue(validEnd.canEnd, "preflightEndAsyncFlow allows paired finish");
               "preflightEndAsyncFlow clears underflow flag for paired finish");
        fuse::profiler::preflightBeginAsyncFlow("disabled_flow", flowId);
    expectTrue(!disabledBegin.canBegin, "preflightBeginAsyncFlow blocks when profiler disabled");
void testChromeTraceExportPreflightExportableOnly() {
    expectTrue(emptyPreflight.hasOnlyExportableEvents(),
    expectTrue(validPreflight.hasOnlyExportableEvents(),
    expectTrue(validPreflight.canExportWithEvents(),
    expectTrue(validPreflight.canExportSafely(),
    const fuse::profiler::ChromeTraceExportPreflight guardedPreflight =
    expectTrue(guardedPreflight.hasOnlyExportableEvents(),
    expectTrue(guardedPreflight.canExportWithEvents(),
    expectTrue(guardedPreflight.eventCount == validPreflight.eventCount,
    testChromeTraceExportPreflightExportableOnly();

// --- deepen additive from deepen-b16-profiler-name-flow-lookup-0de5 ---
void testExportableEventIndexGuard() {
               "tryLastExportableEvent copies last scope end phase");
void testActiveScopeAndFlowNestingGuards() {
    expectTrue(fuse::profiler::tryFirstEventByName("name_lookup_inner", outEvent),
    expectTrue(fuse::profiler::tryLastEventByName("name_lookup_outer", outEvent),
               "tryLastEventByName succeeds for outer end");
               "tryLastEventByName copies outer end phase");
    expectTrue(fuse::profiler::tryFirstEventByFlowId(outerFlowId, outEvent),
               "tryFirstEventByFlowId succeeds for outer start");
    expectTrue(fuse::profiler::tryLastEventByFlowId(innerFlowId, outEvent),
               "tryLastEventByFlowId succeeds for inner finish");

// --- deepen additive from deepen-b16-profiler-name-flow-lookup-6e19 ---
    expectTrue(!fuse::profiler::tryFirstEventByFlowId(1u, outEvent),
               "tryFirstEventByFlowId false on empty buffer");
    expectTrue(fuse::profiler::tryFirstEventByName("try_lookup_flow", outEvent),
               "tryFirstEventByName succeeds for flow start");
               "tryFirstEventByName copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFirstEventByName preserves flow id");
    expectTrue(fuse::profiler::tryLastEventByName("try_lookup_flow", outEvent),
               "tryLastEventByName succeeds for flow finish");
               "tryLastEventByName copies flow finish phase");
               "tryFirstEventByName succeeds for counter");
               "tryFirstExportableEvent succeeds after recording");
               "tryLastExportableEvent succeeds after recording");
void testPreflightNestingStateGuard() {
    expectTrue(resetPreflight.isScopeNestingBalanced(), "nesting preflight balanced on reset");
    expectTrue(resetPreflight.isFlowNestingBalanced(), "flow nesting preflight balanced on reset");
    expectTrue(!resetPreflight.hasOpenAsyncFlows, "nesting preflight has no open flows on reset");
        const fuse::profiler::NestingStatePreflight flowPreflight = fuse::profiler::preflightNestingState();
        expectTrue(flowPreflight.activeFlowNestingDepth == 1u,
        expectTrue(flowPreflight.openAsyncFlowCount == 1u,
    expectTrue(closedPreflight.isScopeNestingBalanced(), "nesting preflight balanced after teardown");
    expectTrue(closedPreflight.isFlowNestingBalanced(), "flow nesting preflight balanced after teardown");
void testPreflightAsyncFlowBeginEndGuard() {
    const fuse::profiler::AsyncFlowBeginPreflight validBegin = fuse::profiler::preflightBeginAsyncFlow("valid_flow");
    const fuse::profiler::AsyncFlowEndPreflight validEnd = fuse::profiler::preflightEndAsyncFlow("valid_flow");
    const fuse::profiler::AsyncFlowBeginPreflight disabledBegin = fuse::profiler::preflightBeginAsyncFlow("disabled");
    testPreflightNestingStateGuard();
    testPreflightAsyncFlowBeginEndGuard();

// --- deepen additive from deepen-b16-profiler-name-flow-guards-2034 ---
void testIsValidFlowIdGuard() {
    expectTrue(!fuse::profiler::tryFindFirstExportableEventByName(nullptr, outEvent),
               "tryFindFirstExportableEventByName false for null name");
    expectTrue(!fuse::profiler::tryFindFirstExportableEventByFlowId(0u, outEvent),
               "tryFindFirstExportableEventByFlowId false for invalid flow id");
    expectTrue(fuse::profiler::tryFindFirstExportableEventByName("lookup_scope", outEvent),
               "tryFindFirstExportableEventByName succeeds for scope begin");
    expectTrue(fuse::profiler::tryFindFirstExportableEventByFlowId(flowId, outEvent),
               "tryFindFirstExportableEventByFlowId succeeds for flow start");
    expectTrue(!fuse::profiler::tryFindFirstExportableEventByName("missing_track", outEvent),
               "tryFindFirstExportableEventByName false for missing name");
void testChromeTraceExportPreflightPairedEventCounts() {
    expectTrue(!emptyPreflight.hasUnpairedScopeEvents, "empty preflight has paired scope events");
    expectTrue(!emptyPreflight.hasUnpairedFlowEvents, "empty preflight has paired flow events");
        expectTrue(activePreflight.scopeEndEventCount == 0u,
        expectTrue(activePreflight.flowFinishEventCount == 0u,
        expectTrue(activePreflight.hasUnpairedScopeEvents,
        expectTrue(activePreflight.hasUnpairedFlowEvents,
        expectTrue(activePreflight.exportableEventCount == 3u,
    expectTrue(openFlowPreflight.scopeBeginEventCount == 1u,
    expectTrue(openFlowPreflight.scopeEndEventCount == 1u,
    expectTrue(openFlowPreflight.flowStartEventCount == 1u,
    expectTrue(openFlowPreflight.flowFinishEventCount == 0u,
    expectTrue(!openFlowPreflight.hasUnpairedScopeEvents,
    expectTrue(openFlowPreflight.hasUnpairedFlowEvents,
    expectTrue(closedPreflight.scopeEndEventCount == 1u,
    expectTrue(!closedPreflight.hasUnpairedScopeEvents,
    expectTrue(closedPreflight.exportableEventCount == 5u,
    testChromeTraceExportPreflightPairedEventCounts();

// --- deepen additive from deepen-b16-profiler-guards-b5ca ---
void testTryExportableEventByNameAndFlowIdGuard() {
    expectTrue(!fuse::profiler::tryFirstExportableEventByName(nullptr, outEvent),
               "tryFirstExportableEventByName false for null name");
               "tryFirstExportableEventByName clears output for null name");
    expectTrue(!fuse::profiler::tryLastExportableEventByName("", outEvent),
               "tryLastExportableEventByName false for empty name");
    expectTrue(!fuse::profiler::tryFirstExportableEventByFlowId(42u, outEvent),
               "tryFirstExportableEventByFlowId false on empty buffer");
    expectTrue(fuse::profiler::tryFirstExportableEventByName("lookup_scope", outEvent),
               "tryFirstExportableEventByName true for scope begin");
               "tryFirstExportableEventByName copies scope begin phase");
               "tryFirstExportableEventByName copies scope name");
    expectTrue(fuse::profiler::tryLastExportableEventByName("lookup_scope", outEvent),
               "tryLastExportableEventByName true for scope end");
               "tryLastExportableEventByName copies scope end phase");
    expectTrue(fuse::profiler::tryFirstExportableEventByFlowId(flowId, outEvent),
               "tryFirstExportableEventByFlowId true for flow start");
               "tryFirstExportableEventByFlowId copies flow start phase");
    expectTrue(outEvent.scopeId == flowId, "tryFirstExportableEventByFlowId copies flow id");
    expectTrue(fuse::profiler::tryLastExportableEventByFlowId(flowId, outEvent),
               "tryLastExportableEventByFlowId true for flow finish");
               "tryLastExportableEventByFlowId copies flow finish phase");
    const fuse::profiler::ProfileScopePreflight emptyNamePreflight =
    expectTrue(emptyNamePreflight.invalidName, "scope preflight marks empty name invalid");
    expectTrue(!emptyNamePreflight.canRecord(), "scope preflight blocks empty name");
    const fuse::profiler::ProfileScopePreflight nullNamePreflight =
    expectTrue(nullNamePreflight.invalidName, "scope preflight marks null name invalid");
    expectTrue(!nullNamePreflight.canRecord(), "scope preflight blocks null name");
    expectTrue(validPreflight.canRecord(), "scope preflight allows valid name");
    expectTrue(!validPreflight.profilerDisabled, "scope preflight profiler enabled on reset");
    expectTrue(!validPreflight.invalidName, "scope preflight clears invalidName for valid name");
    expectTrue(disabledPreflight.profilerDisabled, "scope preflight marks profiler disabled");
    expectTrue(!disabledPreflight.canRecord(), "scope preflight blocks when profiler disabled");
    const fuse::profiler::AsyncFlowBeginPreflight emptyBeginPreflight =
    expectTrue(emptyBeginPreflight.invalidName, "flow begin preflight marks empty name invalid");
    expectTrue(!emptyBeginPreflight.canBegin(), "flow begin preflight blocks empty name");
        fuse::profiler::preflightEndAsyncFlow("orphan_flow", 1u);
    expectTrue(orphanEndPreflight.orphanFinish, "flow end preflight marks orphan finish");
    expectTrue(!orphanEndPreflight.canEnd(), "flow end preflight blocks orphan finish");
    const fuse::profiler::AsyncFlowBeginPreflight validBeginPreflight =
    expectTrue(validBeginPreflight.canBegin(), "flow begin preflight allows valid name");
        fuse::profiler::preflightEndAsyncFlow("valid_flow", flowId);
    expectTrue(!matchedEndPreflight.orphanFinish, "flow end preflight clears orphan with open flow");
    expectTrue(matchedEndPreflight.canEnd(), "flow end preflight allows matched end");
    const fuse::profiler::AsyncFlowBeginPreflight disabledBeginPreflight =
        fuse::profiler::preflightBeginAsyncFlow("disabled_flow");
    expectTrue(disabledBeginPreflight.profilerDisabled, "flow begin preflight marks profiler disabled");
    expectTrue(!disabledBeginPreflight.canBegin(), "flow begin preflight blocks when profiler disabled");
    const fuse::profiler::AsyncFlowEndPreflight disabledEndPreflight =
        fuse::profiler::preflightEndAsyncFlow("disabled_flow", flowId);
    expectTrue(disabledEndPreflight.profilerDisabled, "flow end preflight marks profiler disabled");
    expectTrue(!disabledEndPreflight.canEnd(), "flow end preflight blocks when profiler disabled");
void testChromeTraceExportPreflightBlocksInvalidNamesInSafeExport() {
    expectTrue(balancedPreflight.canExportSafely(),
    const fuse::profiler::ChromeTraceExportPreflight tracePreflight =
    expectTrue(tracePreflight.canExportSafely(),
    expectTrue(!tracePreflight.hasInvalidNameEvents,
    testChromeTraceExportPreflightBlocksInvalidNamesInSafeExport();

// --- deepen additive from b16-profiler-deepen-guards-54d4 ---
    expectTrue(fuse::profiler::tryFindFirstEventByFlowId(outerFlowId, flowEvent),
               "tryFindFirstEventByFlowId clears output on empty buffer");
