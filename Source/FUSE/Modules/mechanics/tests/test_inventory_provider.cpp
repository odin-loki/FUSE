#include <fuse/core/init.hpp>
#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/inventory_provider.hpp>
#include <fuse/types.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

class TestInventoryProvider : public fuse::mechanics::Component,
                              public fuse::mechanics::IInventoryProvider {
public:
    TestInventoryProvider() = default;

    const char* typeName() const override { return "TestInventoryProvider"; }

    void registerInterfaces(fuse::mechanics::Component* owner) override {
        fuse::mechanics::Component::registerInterfaces(owner);
        owner->registerCachedInterface("mechanics", "inventory", this, &m_inventoryInterface);
    }

    bool hasItem(const std::string& item) const override {
        return getItemCount(item) > 0;
    }

    ::fuse::u32 getItemCount(const std::string& item) const override {
        const auto it = m_counts.find(item);
        return it == m_counts.end() ? 0 : it->second;
    }

    ::fuse::u32 grantItem(const std::string& item, ::fuse::u32 amount) override {
        m_counts[item] += amount;
        return amount;
    }

    ::fuse::u32 consumeItem(const std::string& item, ::fuse::u32 amount) override {
        const ::fuse::u32 total = getItemCount(item);
        if (total < amount) {
            amount = total;
        }
        m_counts[item] = total - amount;
        return amount;
    }

private:
    fuse::mechanics::InventoryProviderInterface m_inventoryInterface;
    std::unordered_map<std::string, ::fuse::u32> m_counts;
};

void testInventoryProviderInterface() {
    fuse::mechanics::Component player("player");
    TestInventoryProvider inventory;

    expectTrue(player.addComponent(&inventory), "player accepts inventory child");
    expectTrue(player.attach(), "inventory hierarchy attaches");

    auto* iface = player.getInterface<fuse::mechanics::InventoryProviderInterface>(
        "mechanics", "inventory", &inventory);
    expectTrue(iface != nullptr, "player resolves inventory interface");
    expectTrue(iface->grantItem("key", 1) == 1, "inventory interface grants item");
    expectTrue(iface->hasItem("key"), "inventory interface reports item");
    expectTrue(iface->consumeItem("key", 1) == 1, "inventory interface consumes item");
    expectTrue(!iface->hasItem("key"), "inventory empty after consume");
}

} // namespace

int main() {
    fuse::core::initialize();
    testInventoryProviderInterface();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics inventory provider tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics inventory provider tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
