// FUSE Relight RL-3.6: the particle options (see particle_options.hpp).
#include <fuse/relight/particles/particle_options.hpp>

#include <fuse/relight/options/option_manager.hpp>

#include <variant>

namespace fuse::relight::particles {

namespace {

template <typename T>
bool readByName(const char* name, T& out) {
    if (const options::OptionBase* o = options::OptionManager::findOption(name)) {
        const options::OptionValue v = o->getResolvedValue();
        if (const T* p = std::get_if<T>(&v)) {
            out = *p;
            return true;
        }
    }
    return false;
}

const options::OptionBase* const kRegistered[] = {
    &ParticleOptions::enable, &ParticleOptions::enableSpawning, &ParticleOptions::timeScale,
    &ParticleOptions::enableDiscontinuityGuard, &ParticleOptions::discontinuityFactor,
    &ParticleOptions::discontinuityFloor, &ParticleOptions::spawnRatePerSecond, &ParticleOptions::spawnBurstDuration,
    &ParticleOptions::numberOfParticlesPerMaterial, &ParticleOptions::minParticleLife, &ParticleOptions::maxParticleLife,
    &ParticleOptions::minSpawnSize, &ParticleOptions::maxSpawnSize, &ParticleOptions::minSpawnRotationSpeed,
    &ParticleOptions::maxSpawnRotationSpeed, &ParticleOptions::minSpawnColor, &ParticleOptions::maxSpawnColor,
    &ParticleOptions::minTargetSize, &ParticleOptions::maxTargetSize, &ParticleOptions::minTargetRotationSpeed,
    &ParticleOptions::maxTargetRotationSpeed, &ParticleOptions::minTargetColor, &ParticleOptions::maxTargetColor,
    &ParticleOptions::initialVelocityFromMotion, &ParticleOptions::initialVelocityFromNormal,
    &ParticleOptions::initialVelocityConeAngleDegrees, &ParticleOptions::gravityForce, &ParticleOptions::maxSpawnVelocity,
    &ParticleOptions::maxTargetVelocity, &ParticleOptions::useSpawnTexcoords, &ParticleOptions::alignParticlesToVelocity,
    &ParticleOptions::enableCollisionDetection, &ParticleOptions::collisionRestitution, &ParticleOptions::collisionThickness,
    &ParticleOptions::useTurbulence, &ParticleOptions::turbulenceForce, &ParticleOptions::turbulenceFrequency,
    &ParticleOptions::enableMotionTrail, &ParticleOptions::motionTrailMultiplier, &ParticleOptions::billboardType,
    &ParticleOptions::spriteSheetMode, &ParticleOptions::attractorPosition, &ParticleOptions::attractorForce,
    &ParticleOptions::attractorRadius, &ParticleOptions::collisionMode, &ParticleOptions::dragCoefficient,
    &ParticleOptions::restrictVelocityX, &ParticleOptions::restrictVelocityY, &ParticleOptions::restrictVelocityZ,
    &ParticleOptions::randomFlipAxis, &ParticleOptions::initialRotationDeviationDegrees,
};

} // namespace

float ParticleOptions::sceneScale() {
    float v = 1.f;
    readByName("rtx.sceneScale", v);
    return v;
}

float ParticleOptions::resolveTransparencyThreshold() {
    float v = 1.f / 255.f;
    readByName("rtx.resolveTransparencyThreshold", v);
    return v;
}

options::Vec3f ParticleOptions::sceneUp() {
    bool zUp = false;
    readByName("rtx.zUp", zUp);
    return zUp ? Vec3f(0.f, 0.f, 1.f) : Vec3f(0.f, 1.f, 0.f);
}

void registerParticleOptions() {
    for (const options::OptionBase* o : kRegistered) {
        (void)o;
    }
}

} // namespace fuse::relight::particles
