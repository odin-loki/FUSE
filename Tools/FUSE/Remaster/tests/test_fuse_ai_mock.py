"""W0.5 exit criterion: mock providers are deterministic (plan §3.1 ``mock`` row, §7 Wave 0)."""

import hashlib
import unittest

from tests.fuse_ai_support import tiny_png

from fuse_ai import imaging, mock, providers
from fuse_ai.providers import InputRef, Request, Task

PORT_SOURCE = "function onAdd(%this, %obj)\n{\n   echo(%obj);\n}\nfunction Player::jump(%this)\n{\n}\n"

# Pinned sha256s of a few mock outputs. A change here means the mock output drifted (platform, Python
# version or an intentional change to fuse_ai.mock - in that case re-pin and say so in the commit).
PINNED = {
    "UPSCALE": "6467b3fdd628da60b1ca7a340da47719bc5e4a51ac0ca016aa767e9139055a05",
    "PBR_MAPS": "373ebf9285920f8c0d992b2e369af16d0bd1614acff511286a6583262b218374",
    "PORT_SCRIPT": "71a5c8d1410c62ee99d0a8a692b3bdf28d993371c5369d722540ca9f8fa17ef3",
    "WORLD_ROLLOUT": "4389477abcf6c9234b6a70ad9ea14301e8c8b603196fac60c214f2a7bfd30591",
    "TTS_SYNTH": "ba53e6ae6cb355b24aac36b9be4f6c6dc6a0d248a99e2993928ff83df8568082",
}


def _request(task: Task) -> Request:
    params = {}
    data = tiny_png(b"pin", 8)
    if task in (Task.PORT_SCRIPT, Task.SUMMARISE_CODE):
        data = PORT_SOURCE.encode("utf-8")
    if task in (Task.TRANSCRIBE, Task.SEPARATE_STEMS, Task.AUDIO_SR):
        data = imaging.encode_wav(imaging.synthetic_audio(b"pin", 8000, 50), 8000)
    if task is Task.TTS_SYNTH:
        params["text"] = "Hello FUSE"
    if task in (Task.GEN_IMAGE, Task.GEN_VIDEO, Task.EDIT_IMAGE):
        params["prompt"] = "a mossy stone arch"
    ref = InputRef(canonical_sha256=hashlib.sha256(data).hexdigest(), origin="library", name="rusty_metal_panel")
    return Request(task=task, provider_id="mock", params=params, seed=7, inputs=[ref], input_data=[data])


def _run(task: Task):
    return providers.get("mock").run(_request(task))


class MockDeterminism(unittest.TestCase):
    def test_every_task_has_a_mock(self):
        self.assertEqual(providers.get("mock").capabilities(), set(Task))
        for t in Task:
            self.assertIn(t, mock.HANDLERS)

    def test_run_twice_identical_bytes(self):
        for t in Task:
            with self.subTest(task=t.value):
                a, b = _run(t), _run(t)
                self.assertGreater(len(a.outputs), 0)
                self.assertEqual([(o.name, o.media_type, o.data) for o in a.outputs],
                                 [(o.name, o.media_type, o.data) for o in b.outputs])
                self.assertEqual(a.meta, b.meta)

    def test_pinned_hashes(self):
        got = {name: hashlib.sha256(_run(Task(name)).primary.data).hexdigest() for name in PINNED}
        self.assertEqual(got, PINNED)

    def test_mock_never_costs(self):
        p = providers.get("mock")
        req = _request(Task.UPSCALE)
        self.assertEqual(p.estimate(req).usd, 0.0)
        self.assertEqual(p.vram_mb(req), 0)


class MockBehaviour(unittest.TestCase):
    def test_png_round_trip(self):
        img = imaging.synthetic_image(b"rt", 7, 5)
        self.assertEqual(imaging.decode_png(imaging.encode_png(img)), img)

    def test_upscale_x4_size_and_flat_colour(self):
        flat = imaging.Image(3, 2, bytes((10, 20, 30, 255)) * 6)
        up = mock.bicubic_upscale(flat, 4)
        self.assertEqual((up.width, up.height), (12, 8))
        self.assertEqual(up.rgba, bytes((10, 20, 30, 255)) * 96)  # unit-gain fixed-point weights

    def test_upscale_default_scale_is_4(self):
        r = providers.get("mock").run(Request(task=Task.UPSCALE, provider_id="mock",
                                              inputs=[InputRef(sha256="2" * 64, origin="library")],
                                              input_data=[tiny_png(b"s", 4)]))
        img = imaging.decode_png(r.primary.data)
        self.assertEqual((img.width, img.height), (16, 16))

    def test_sobel_normal_of_flat_image_points_up(self):
        flat = imaging.Image(4, 4, bytes((90, 90, 90, 255)) * 16)
        n = mock.sobel_normal_map(flat)
        self.assertEqual(n.pixel(1, 1), (128, 128, 255, 255))

    def test_pbr_outputs(self):
        names = [o.name for o in _run(Task.PBR_MAPS).outputs]
        self.assertEqual(names, ["normal.png", "roughness.png", "height.png"])

    def test_tag_keyword_table(self):
        import json
        tags = json.loads(_run(Task.TAG_IMAGE).primary.data)["tags"]
        self.assertIn("metal", tags)
        self.assertIn("rust", tags)

    def test_port_script_is_canned_lua(self):
        lua = _run(Task.PORT_SCRIPT).primary.data.decode()
        self.assertIn("function M.onAdd(this, obj)", lua)
        self.assertIn("function M.Player_jump(this)", lua)
        self.assertTrue(lua.rstrip().endswith("return M"))

    def test_stems_sum_to_input(self):
        req = _request(Task.SEPARATE_STEMS)
        src, _, _ = imaging.decode_wav(req.input_data[0])
        stems = [imaging.decode_wav(o.data)[0] for o in providers.get("mock").run(req).outputs]
        self.assertEqual([sum(s[i] for s in stems) for i in range(len(src))], src)

    def test_world_rollout_fixed_regardless_of_seed(self):
        a = _request(Task.WORLD_ROLLOUT)
        b = _request(Task.WORLD_ROLLOUT)
        b.seed = 12345
        pa, pb = providers.get("mock").run(a), providers.get("mock").run(b)
        self.assertEqual(pa.primary.data, pb.primary.data)
        self.assertEqual(len(pa.outputs), 9)  # manifest + 8 frames

    def test_absent_input_bytes_are_synthesised_from_the_hash(self):
        req = Request(task=Task.PBR_MAPS, provider_id="mock", inputs=[InputRef(canonical_sha256="3" * 64)],
                      input_data=[None])
        self.assertEqual(providers.get("mock").run(req).primary.data,
                         providers.get("mock").run(req).primary.data)


if __name__ == "__main__":
    unittest.main()
