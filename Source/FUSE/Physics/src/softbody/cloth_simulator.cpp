#include <fuse/physics/softbody/cloth_simulator.hpp>

#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {

namespace {

/// Parallel bands only pay off once the cloth is large enough to amortise the dispatch.
constexpr u32 kMinParticlesForBands = 1024u;
constexpr u32 kMinRowsPerBand = 8u;

} // namespace

void ClothSimulator::init(const ClothDesc& desc, vec3 origin) {
    destroy();
    if (desc.rows < 2 || desc.cols < 2) {
        return;
    }

    m_desc = desc;
    m_desc.substeps = std::max(1u, desc.substeps);
    m_desc.iterations = std::max(1u, desc.iterations);
    const u32 particleCount = desc.rows * desc.cols;
    m_particles = ParticleSoA::allocate(particleCount);
    m_particles.count = particleCount;

    const f32 invMass = desc.particleMass > 0.f ? 1.f / desc.particleMass : 1.f;
    for (u32 row = 0; row < desc.rows; ++row) {
        for (u32 col = 0; col < desc.cols; ++col) {
            const u32 i = index(row, col);
            m_particles.positions[i] = {
                origin.x + static_cast<f32>(col) * desc.particleSpacing,
                origin.y,
                origin.z + static_cast<f32>(row) * desc.particleSpacing,
            };
            m_particles.prevPositions[i] = m_particles.positions[i];
            m_particles.velocities[i] = {};
            m_particles.invMasses[i] = invMass;
        }
    }

    const u32 lastRow = desc.rows - 1;
    const u32 lastCol = desc.cols - 1;
    if (desc.pinnedCorners & 0b0001u) {
        m_particles.invMasses[index(0, 0)] = 0.f;
    }
    if (desc.pinnedCorners & 0b0010u) {
        m_particles.invMasses[index(0, lastCol)] = 0.f;
    }
    if (desc.pinnedCorners & 0b0100u) {
        m_particles.invMasses[index(lastRow, 0)] = 0.f;
    }
    if (desc.pinnedCorners & 0b1000u) {
        m_particles.invMasses[index(lastRow, lastCol)] = 0.f;
    }

    buildConstraints_();
    m_vertexBuffer = 1;
    m_indexBuffer = 2;
    m_indexCount = (desc.rows - 1) * (desc.cols - 1) * 6;
    m_ready = true;
}

void ClothSimulator::destroy() {
    ParticleSoA::free(m_particles);
    m_constraints.clear();
    m_tethers.clear();
    m_packed.clear();
    m_bandStarts.clear();
    m_packedDt = 0.f;
    m_structuralCount = 0;
    m_spheres.clear();
    m_boxes.clear();
    m_wind = {};
    m_vertexBuffer = 0;
    m_indexBuffer = 0;
    m_indexCount = 0;
    m_ready = false;
}

void ClothSimulator::applyWind(vec3 windVelocity) {
    m_wind = windVelocity;
}

void ClothSimulator::addSphereCollider(vec3 center, f32 radius) {
    m_spheres.push_back({center, radius});
}

void ClothSimulator::addBoxCollider(vec3 center, vec3 halfExtents) {
    if (halfExtents.x <= 0.f || halfExtents.y <= 0.f || halfExtents.z <= 0.f) {
        return;
    }
    m_boxes.push_back({center, halfExtents});
}

void ClothSimulator::step(f32 dt, vec3 gravity) {
    if (!m_ready || dt <= 0.f) {
        return;
    }
    const f32 h = dt / static_cast<f32>(m_desc.substeps);
    const f32 damping = std::pow(std::clamp(m_desc.damping, 0.f, 1.f), h);
    const u32 count = m_particles.count;

    applyAerodynamics_(dt);
    const f32 invH = 1.f / h;
    const vec3 gravityStep = gravity * h;
    vec3* const positions = m_particles.positions;
    vec3* const prevPositions = m_particles.prevPositions;
    vec3* const velocities = m_particles.velocities;
    const f32* const invMasses = m_particles.invMasses;
    for (u32 substep = 0; substep < m_desc.substeps; ++substep) {
        // Velocity of the previous substep (from its solved positions) fused with this substep's
        // prediction: one pass over the particles per substep.
        const bool first = substep == 0u;
        for (u32 i = 0; i < count; ++i) {
            if (invMasses[i] == 0.f) {
                velocities[i] = {};
                prevPositions[i] = positions[i];
                continue;
            }
            vec3 v = first ? velocities[i] : (positions[i] - prevPositions[i]) * (invH * damping);
            v += gravityStep;
            velocities[i] = v;
            prevPositions[i] = positions[i];
            positions[i] += v * h;
        }
        for (u32 iteration = 0; iteration < m_desc.iterations; ++iteration) {
            solveConstraints_(h);
        }
        solveTethers_();
        collideSpheres_();
        collideBoxes_();
    }
    for (u32 i = 0; i < count; ++i) {
        velocities[i] = invMasses[i] == 0.f ? vec3{} : (positions[i] - prevPositions[i]) * (invH * damping);
    }
}

void ClothSimulator::applyAerodynamics_(f32 dt) {
    if (m_desc.windDrag <= 0.f) {
        return;
    }
    // Per-particle drag along the grid vertex normal: F = k * A * ((wind - v) . n) n with A the
    // particle's share of cloth area. Each particle writes only its own velocity (parallel-safe).
    const u32 rows = m_desc.rows;
    const u32 cols = m_desc.cols;
    const f32 area = m_desc.particleSpacing * m_desc.particleSpacing;
    const vec3* const positions = m_particles.positions;
    auto rowBody = [&](u32 row) {
        const u32 up = row > 0u ? row - 1u : row;
        const u32 down = row + 1u < rows ? row + 1u : row;
        for (u32 col = 0; col < cols; ++col) {
            const u32 i = index(row, col);
            if (m_particles.invMasses[i] == 0.f) {
                continue;
            }
            const u32 left = col > 0u ? col - 1u : col;
            const u32 right = col + 1u < cols ? col + 1u : col;
            const vec3 across = positions[index(row, right)] - positions[index(row, left)];
            const vec3 along = positions[index(down, col)] - positions[index(up, col)];
            const vec3 normal = across.cross(along);
            const f32 length = normal.length();
            if (length < 1e-12f) {
                continue;
            }
            const vec3 n = normal * (1.f / length);
            const f32 relative = (m_wind - m_particles.velocities[i]).dot(n);
            m_particles.velocities[i] += n * (m_desc.windDrag * area * relative * m_particles.invMasses[i] * dt);
        }
    };
    const auto& scheduler = fuse::jobs::JobScheduler::instance();
    if (m_particles.count >= kMinParticlesForBands && scheduler.isInitialized() && !scheduler.isSingleThreaded()) {
        fuse::jobs::parallel_for(0u, rows, kMinRowsPerBand, rowBody);
    } else {
        for (u32 row = 0; row < rows; ++row) {
            rowBody(row);
        }
    }
}

void ClothSimulator::prepareSolve_(f32 dt) {
    if (m_packedDt == dt && !m_bandStarts.empty()) {
        return;
    }
    const auto& scheduler = fuse::jobs::JobScheduler::instance();
    u32 bandCount = 1u;
    if (m_particles.count >= kMinParticlesForBands && scheduler.isInitialized() && !scheduler.isSingleThreaded()) {
        bandCount = std::clamp(scheduler.workerCount() + 1u, 1u, std::max(1u, m_desc.rows / kMinRowsPerBand));
    }
    const u32 rows = m_desc.rows;
    const u32 cols = m_desc.cols;
    auto bandOf = [&](u32 particle) { return (particle / cols) * bandCount / rows; };

    const f32 invDt2 = 1.f / (dt * dt);
    std::vector<std::vector<PackedConstraint>> bands(bandCount + 1u); // last bucket: crossings
    for (const ParticleConstraint& constraint : m_constraints) {
        const f32 wA = m_particles.invMasses[constraint.a];
        const f32 wB = m_particles.invMasses[constraint.b];
        const f32 wSum = wA + wB;
        if (wSum <= 0.f) {
            continue; // both ends pinned
        }
        const f32 denom = wSum + constraint.compliance * invDt2;
        const u32 bandA = bandOf(constraint.a);
        const u32 bucket = bandA == bandOf(constraint.b) ? bandA : bandCount;
        bands[bucket].push_back({constraint.a, constraint.b, constraint.restLength, wA / denom, wB / denom});
    }
    m_packed.clear();
    m_bandStarts.clear();
    // Within a bucket, order the constraints colour by colour (greedy: no two constraints of a
    // colour share a particle). Consecutive constraints then touch different particles, so the
    // CPU overlaps their square roots and divides instead of waiting on the previous store.
    std::vector<u64> usedColours(m_particles.count, 0u);
    std::vector<u32> colours;
    std::vector<u32> colourCounts;
    for (const auto& band : bands) {
        m_bandStarts.push_back(static_cast<u32>(m_packed.size()));
        colours.assign(band.size(), 0u);
        colourCounts.assign(65u, 0u);
        for (usize i = 0; i < band.size(); ++i) {
            const u64 used = usedColours[band[i].a] | usedColours[band[i].b];
            u32 colour = 0;
            while (colour < 64u && (used & (u64{1} << colour)) != 0u) {
                ++colour;
            }
            colours[i] = colour;
            ++colourCounts[colour];
            if (colour < 64u) {
                usedColours[band[i].a] |= u64{1} << colour;
                usedColours[band[i].b] |= u64{1} << colour;
            }
        }
        for (const PackedConstraint& c : band) {
            usedColours[c.a] = 0u;
            usedColours[c.b] = 0u;
        }
        std::vector<u32> offsets(65u, 0u);
        for (u32 colour = 1; colour < 65u; ++colour) {
            offsets[colour] = offsets[colour - 1u] + colourCounts[colour - 1u];
        }
        const usize base = m_packed.size();
        m_packed.resize(base + band.size());
        for (usize i = 0; i < band.size(); ++i) {
            m_packed[base + offsets[colours[i]]++] = band[i];
        }
    }
    m_packedDt = dt;
}

void ClothSimulator::solveRange_(u32 begin, u32 end) {
    vec3* const positions = m_particles.positions;
    for (u32 i = begin; i < end; ++i) {
        const PackedConstraint& c = m_packed[i];
        vec3& pa = positions[c.a];
        vec3& pb = positions[c.b];
        const f32 dx = pa.x - pb.x;
        const f32 dy = pa.y - pb.y;
        const f32 dz = pa.z - pb.z;
        const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < 1e-9f) {
            continue;
        }
        const f32 scale = (c.restLength - dist) / dist;
        const f32 sA = scale * c.kA;
        const f32 sB = scale * c.kB;
        pa.x += dx * sA;
        pa.y += dy * sA;
        pa.z += dz * sA;
        pb.x -= dx * sB;
        pb.y -= dy * sB;
        pb.z -= dz * sB;
    }
}

void ClothSimulator::solveConstraints_(f32 dt) {
    prepareSolve_(dt);
    const u32 bandCount = static_cast<u32>(m_bandStarts.size()) - 1u;
    if (bandCount > 1u) {
        fuse::jobs::parallel_for(0u, bandCount, 1u,
                                 [&](u32 band) { solveRange_(m_bandStarts[band], m_bandStarts[band + 1u]); });
    } else {
        solveRange_(m_bandStarts[0], m_bandStarts[1]);
    }
    solveRange_(m_bandStarts[bandCount], static_cast<u32>(m_packed.size()));
}

void ClothSimulator::collideSpheres_() {
    for (const ClothSphereCollider& sphere : m_spheres) {
        const f32 minDist = sphere.radius + m_desc.thickness;
        for (u32 i = 0; i < m_particles.count; ++i) {
            if (m_particles.invMasses[i] == 0.f) {
                continue;
            }
            const vec3 offset = m_particles.positions[i] - sphere.center;
            const f32 dist = offset.length();
            if (dist >= minDist || dist < 1e-9f) {
                continue;
            }
            const vec3 n = offset * (1.f / dist);
            vec3 resolved = sphere.center + n * minDist;
            // Friction: drop part of this substep's slip along the surface.
            const vec3 slip = resolved - m_particles.prevPositions[i];
            const vec3 tangential = slip - n * slip.dot(n);
            resolved -= tangential * std::clamp(m_desc.friction, 0.f, 1.f);
            m_particles.positions[i] = resolved;
        }
    }
}

void ClothSimulator::collideBoxes_() {
    const f32 skin = m_desc.thickness;
    const f32 friction = std::clamp(m_desc.friction, 0.f, 1.f);
    for (const ClothBoxCollider& box : m_boxes) {
        const vec3 expanded{box.halfExtents.x + skin, box.halfExtents.y + skin, box.halfExtents.z + skin};
        for (u32 i = 0; i < m_particles.count; ++i) {
            if (m_particles.invMasses[i] == 0.f) {
                continue;
            }
            const vec3 local = m_particles.positions[i] - box.center;
            const f32 ax = std::fabs(local.x);
            const f32 ay = std::fabs(local.y);
            const f32 az = std::fabs(local.z);
            if (ax >= expanded.x || ay >= expanded.y || az >= expanded.z) {
                continue;
            }

            const f32 penX = expanded.x - ax;
            const f32 penY = expanded.y - ay;
            const f32 penZ = expanded.z - az;
            vec3 normal{local.x < 0.f ? -1.f : 1.f, 0.f, 0.f};
            f32 penetration = penX;
            if (penY < penetration) {
                penetration = penY;
                normal = {0.f, local.y < 0.f ? -1.f : 1.f, 0.f};
            }
            if (penZ < penetration) {
                penetration = penZ;
                normal = {0.f, 0.f, local.z < 0.f ? -1.f : 1.f};
            }

            vec3 resolved = m_particles.positions[i] + normal * penetration;
            const vec3 slip = resolved - m_particles.prevPositions[i];
            const vec3 tangential = slip - normal * slip.dot(normal);
            resolved -= tangential * friction;
            m_particles.positions[i] = resolved;
        }
    }
}

void ClothSimulator::solveTethers_() {
    // Most tethers are slack: compare squared lengths and only take the root when one binds.
    vec3* const positions = m_particles.positions;
    for (const ParticleConstraint& tether : m_tethers) {
        const vec3 offset = positions[tether.b] - positions[tether.a];
        const f32 distSq = offset.dot(offset);
        if (distSq > tether.restLength * tether.restLength) {
            positions[tether.b] = positions[tether.a] + offset * (tether.restLength / std::sqrt(distSq));
        }
    }
}

f32 ClothSimulator::maxStretchRatio() const {
    f32 worst = 0.f;
    for (u32 c = 0; c < m_structuralCount; ++c) {
        const ParticleConstraint& constraint = m_constraints[c];
        const f32 length = (m_particles.positions[constraint.a] - m_particles.positions[constraint.b]).length();
        worst = std::max(worst, length / constraint.restLength);
    }
    return worst;
}

void ClothSimulator::buildConstraints_() {
    m_constraints.clear();
    const u32 rows = m_desc.rows;
    const u32 cols = m_desc.cols;
    const f32 s = m_desc.particleSpacing;
    auto add = [&](u32 a, u32 b, f32 rest, f32 compliance) { m_constraints.push_back({a, b, rest, compliance, 1.f}); };

    // Structural edges first (maxStretchRatio reads this prefix).
    for (u32 row = 0; row < rows; ++row) {
        for (u32 col = 0; col < cols; ++col) {
            if (col + 1 < cols) {
                add(index(row, col), index(row, col + 1), s, m_desc.stretchCompliance);
            }
            if (row + 1 < rows) {
                add(index(row, col), index(row + 1, col), s, m_desc.stretchCompliance);
            }
        }
    }
    m_structuralCount = static_cast<u32>(m_constraints.size());

    const f32 diagonal = s * std::sqrt(2.f);
    for (u32 row = 0; row + 1 < rows; ++row) {
        for (u32 col = 0; col + 1 < cols; ++col) {
            add(index(row, col), index(row + 1, col + 1), diagonal, m_desc.stretchCompliance);
            add(index(row, col + 1), index(row + 1, col), diagonal, m_desc.stretchCompliance);
        }
    }
    m_tethers.clear();
    if (m_desc.longRangeAttachments) {
        for (u32 pin = 0; pin < m_particles.count; ++pin) {
            if (m_particles.invMasses[pin] != 0.f) {
                continue;
            }
            for (u32 i = 0; i < m_particles.count; ++i) {
                if (m_particles.invMasses[i] != 0.f) {
                    const f32 rest = (m_particles.positions[i] - m_particles.positions[pin]).length();
                    m_tethers.push_back({pin, i, rest, 0.f, 1.f});
                }
            }
        }
    }

    for (u32 row = 0; row < rows; ++row) {
        for (u32 col = 0; col < cols; ++col) {
            if (col + 2 < cols) {
                add(index(row, col), index(row, col + 2), 2.f * s, m_desc.bendCompliance);
            }
            if (row + 2 < rows) {
                add(index(row, col), index(row + 2, col), 2.f * s, m_desc.bendCompliance);
            }
        }
    }
}

} // namespace fuse::physics
