#pragma once

#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/interact_action.hpp>
#include <fuse/mechanics/interactable.hpp>

#include <string>

namespace fuse::mechanics {

/// Single-action executor surface (ore: GMK leaf action components).
class IActionStub {
public:
    virtual ~IActionStub() = default;

    virtual InteractActionKind kind() const = 0;
    virtual bool canExecute(const InteractionContext& ctx) const = 0;
    virtual bool execute(InteractionContext& ctx) = 0;

    u32 executionCount() const { return m_executionCount; }
    const std::string& lastItem() const { return m_lastItem; }

protected:
    void recordExecution(const InteractionContext& ctx);

private:
    u32 m_executionCount = 0;
    std::string m_lastItem;
};

/// Records one verb execution — used by built-in action stubs and tests.
class ActionStubComponent : public Component, public IActionStub {
public:
    explicit ActionStubComponent(InteractActionKind kind);
    ActionStubComponent(std::string name, InteractActionKind kind);

    const char* typeName() const override { return "ActionStubComponent"; }

    InteractActionKind kind() const override { return m_kind; }
    bool canExecute(const InteractionContext& ctx) const override;
    bool execute(InteractionContext& ctx) override;

private:
    InteractActionKind m_kind;
};

class UseActionStub : public ActionStubComponent {
public:
    UseActionStub();
    explicit UseActionStub(std::string name);

    const char* typeName() const override { return "UseActionStub"; }
};

class PickupActionStub : public ActionStubComponent {
public:
    PickupActionStub();
    explicit PickupActionStub(std::string name);

    const char* typeName() const override { return "PickupActionStub"; }
};

class ExamineActionStub : public ActionStubComponent {
public:
    ExamineActionStub();
    explicit ExamineActionStub(std::string name);

    const char* typeName() const override { return "ExamineActionStub"; }
};

} // namespace fuse::mechanics
