#include <fuse/net/serializer.hpp>

#include "test_helpers.hpp"

#include <cstring>

namespace fuse::net::tests {

void run_serializer_tests() {
    fuse::net::NetSerializer writer;
    writer.write_u8(7);
    writer.write_u16(0xABCD);
    writer.write_u32(0x12345678u);
    writer.write_f32(3.5f);
    writer.write_vec3({1.f, 2.f, 3.f, 0.f});
    writer.write_quat({0.f, 0.f, 0.f, 1.f});
    writer.write_str("fuse");

    fuse::net::NetSerializer reader;
    reader.buffer = writer.buffer;
    reader.reset_read();

    expectTrue(reader.read_u8() == 7, "u8 round-trip");
    expectTrue(reader.read_u16() == 0xABCD, "u16 round-trip");
    expectTrue(reader.read_u32() == 0x12345678u, "u32 round-trip");
    expectNear(reader.read_f32(), 3.5f, 1e-5f, "f32 round-trip");

    const fuse::ecs::vec3 vec = reader.read_vec3();
    expectNear(vec.x, 1.f, 1e-5f, "vec3.x round-trip");
    expectNear(vec.y, 2.f, 1e-5f, "vec3.y round-trip");
    expectNear(vec.z, 3.f, 1e-5f, "vec3.z round-trip");

    const fuse::ecs::quat quat = reader.read_quat();
    expectNear(quat.w, 1.f, 1e-5f, "quat.w round-trip");

    char text[16] = {};
    reader.read_str(text, sizeof(text));
    expectTrue(std::strcmp(text, "fuse") == 0, "string round-trip");
    expectTrue(reader.read_complete(), "serializer fully consumed");
}

} // namespace fuse::net::tests
