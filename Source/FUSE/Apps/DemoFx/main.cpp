#include "demo_check.hpp"

#include <fuse/core/init.hpp>
#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/fx/spell_descriptor.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_fx: fuse_fx composer vertical slice (AFX-inspired)");

    std::string projectPath = "Samples/unification/demo_fx";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.fx, "project enables fuse_fx");

    fuse::fx::FxComposer composer;
    composer.registerEffect(fuse::fx::EffectDescriptor::makeSparkBurst());
    composer.registerEffect(fuse::fx::EffectDescriptor::makeMuzzleFlash());
    composer.registerSpell(fuse::fx::SpellDescriptor::makeFireball());

    fuse::fx::FxSocket spriteSocket;
    spriteSocket.kind = fuse::fx::FxSocketKind::Sprite2D;
    spriteSocket.effectId = "spark_burst";
    fuse::demo::check(composer.attach(spriteSocket), "sprite socket attaches");

    fuse::fx::FxSocket shapeSocket;
    shapeSocket.kind = fuse::fx::FxSocketKind::Shape3D;
    shapeSocket.effectId = "muzzle_flash";
    fuse::demo::check(composer.attach(shapeSocket), "shape socket attaches");

    fuse::fx::CastBinding castBinding;
    castBinding.caster = fuse::Handle<fuse::Object>(1, 1);
    fuse::demo::check(composer.beginCast("fireball", castBinding), "fireball cast begins");

    for (int frame = 0; frame < 5; ++frame) {
        composer.tick();
    }

    fuse::demo::check(composer.attachmentCount() == 2u, "two FX sockets attached");
    fuse::demo::check(composer.effectTimeline().activeCount() == 2u, "effect timeline active");
    fuse::demo::check(composer.castPipeline().activeCount() == 1u, "fireball cast active");
    fuse::demo::check(composer.tickCount() == 5u, "FX composer ticked");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_fx");
}
