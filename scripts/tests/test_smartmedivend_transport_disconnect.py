import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class SmartMediVendTransportDisconnectTest(unittest.TestCase):
    def source(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_protocol_loss_reaches_board_fail_closed_hook(self):
        application = self.source("main/application.cc")
        mqtt = self.source("main/protocols/mqtt_protocol.cc")
        websocket = self.source("main/protocols/websocket_protocol.cc")
        board = self.source("main/boards/smartmedivend-s3/smartmedivend_board.cc")

        self.assertRegex(
            application,
            r"protocol_->OnDisconnected\(\[this\]\(\)\s*\{\s*ReportTransportLoss\(\);\s*\}\);",
        )
        self.assertRegex(
            application,
            r"MAIN_EVENT_TRANSPORT_DISCONNECTED[\s\S]{0,500}"
            r"Board::GetInstance\(\)\.OnNetworkDisconnected\(\);",
        )
        self.assertRegex(
            mqtt,
            r"OnDisconnected\(\[this\]\(\)[\s\S]{0,500}on_disconnected_\(\);",
        )
        self.assertRegex(
            websocket,
            r"OnDisconnected\(\[this\]\(\)[\s\S]{0,500}"
            r"intentional_close_[\s\S]{0,300}on_disconnected_\(\);",
        )
        self.assertRegex(
            board,
            r"void OnNetworkDisconnected\(\) override[\s\S]{0,300}"
            r"coordinator_->OnDisconnected",
        )


if __name__ == "__main__":
    unittest.main()
