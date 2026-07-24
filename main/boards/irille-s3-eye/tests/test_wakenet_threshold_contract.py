import json
from pathlib import Path
import re
import unittest


BOARD_DIR = Path(__file__).resolve().parents[1]
MAIN_DIR = BOARD_DIR.parents[1]
KCONFIG = MAIN_DIR / "Kconfig.projbuild"
AFE_ENGINE = MAIN_DIR / "audio" / "engines" / "afe_audio_engine.cc"


class WakeNetThresholdContractTests(unittest.TestCase):
    def test_board_enables_fifty_three_percent_threshold(self) -> None:
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        sdkconfig = set(config["builds"][0]["sdkconfig_append"])

        self.assertIn("CONFIG_WAKENET_DET_THRESHOLD_PERCENT=53", sdkconfig)

    def test_kconfig_defaults_to_disabled_and_documents_valid_range(self) -> None:
        kconfig = KCONFIG.read_text(encoding="utf-8")
        match = re.search(
            r"config WAKENET_DET_THRESHOLD_PERCENT\n(?P<body>.*?)(?=\nconfig |\nchoice |\Z)",
            kconfig,
            re.DOTALL,
        )

        self.assertIsNotNone(match)
        block = match.group("body")
        self.assertIn("default 0", block)
        self.assertIn("range 0 99", block)
        self.assertIn("depends on USE_AFE_WAKE_WORD", block)
        self.assertIn("40-99", block)

    def test_afe_guard_rejects_invalid_values_and_wraps_runtime_call(self) -> None:
        source = AFE_ENGINE.read_text(encoding="utf-8")
        match = re.search(
            r"#if CONFIG_WAKENET_DET_THRESHOLD_PERCENT > 0(?P<body>.*?)#endif",
            source,
            re.DOTALL,
        )

        self.assertIsNotNone(match)
        block = match.group("body")
        self.assertIn("static_assert", block)
        self.assertIn("CONFIG_WAKENET_DET_THRESHOLD_PERCENT >= 40", block)
        self.assertIn("CONFIG_WAKENET_DET_THRESHOLD_PERCENT <= 99", block)
        self.assertIn("set_wakenet_threshold(afe_data_, 1, threshold)", block)
        self.assertIn("afe_iface_->destroy(afe_data_)", block)
        self.assertIn("return false", block)

        create_position = source.index("create_from_config(afe_config)")
        threshold_position = source.index("set_wakenet_threshold(afe_data_, 1, threshold)")
        task_position = source.index("xTaskCreate(")
        self.assertLess(create_position, threshold_position)
        self.assertLess(threshold_position, task_position)


if __name__ == "__main__":
    unittest.main()
