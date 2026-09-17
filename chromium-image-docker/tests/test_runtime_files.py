from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RuntimeImageFilesTest(unittest.TestCase):
    def test_runtime_files(self) -> None:
        expected = {
            "compose.yaml",
            "image/usr/local/bin/chromium-runtime-entrypoint",
            "image/opt/chromium-runtime/serve.sh",
            "image/opt/chromium-runtime/status.py",
            "image/opt/chromium-runtime/wait.py",
        }
        for relative in expected:
            self.assertTrue((ROOT / relative).exists(), relative)

        self.assertFalse((ROOT / "image/opt/chromium-runtime/capture_site.py").exists())
        self.assertFalse((ROOT / "image/opt/chromium-runtime/smoke_cdp.py").exists())

    def test_entrypoint_exposes_lifecycle_only(self) -> None:
        text = (ROOT / "image/usr/local/bin/chromium-runtime-entrypoint").read_text()
        for command in ["serve)", "status)", "wait)", "version)"]:
            self.assertIn(command, text)
        self.assertNotIn("capture)", text)
        self.assertNotIn("smoke)", text)

    def test_dockerfile_includes_browser_and_agent_runtime(self) -> None:
        text = (ROOT / "Dockerfile").read_text()
        self.assertIn("COPY --chown=wootz:wootz build/ /opt/wootzapp/", text)
        self.assertIn("COPY --chmod=0755 image/ /", text)
        self.assertIn("FROM node:24-bookworm-slim AS node-runtime", text)
        self.assertIn("COPY --from=agent_browser package.json package-lock.json ./", text)
        self.assertIn("npm ci --omit=dev", text)
        self.assertIn("/usr/local/bin/agent-browser", text)
        self.assertNotIn("agent-browser-core", text)
        self.assertNotIn("OPENAI_", text)
        self.assertNotIn("AI_GATEWAY_", text)
        self.assertNotIn("AGENT_BROWSER_MAX_STEPS", text)
        self.assertNotIn("chmod 0755 /opt/wootzapp/chrome", text)
        self.assertIn('ENTRYPOINT ["/usr/local/bin/chromium-runtime-entrypoint"]', text)
        self.assertIn('CMD ["serve"]', text)
        self.assertIn("HEALTHCHECK", text)

    def test_dockerignore_keeps_context_small(self) -> None:
        text = (ROOT / ".dockerignore").read_text()
        self.assertNotIn("!build/**", text)
        self.assertNotIn("!image/**", text)
        self.assertIn("outputs", text)
        self.assertIn("tests", text)
        self.assertIn("scripts", text)
        self.assertIn("compose.yaml", text)

    def test_compose_uses_standard_local_ports(self) -> None:
        text = (ROOT / "compose.yaml").read_text()
        self.assertIn("${CDP_PORT:-9222}:9222", text)
        self.assertIn("${VNC_PORT:-5900}:5900", text)
        self.assertIn("${NOVNC_PORT:-6080}:6080", text)
        self.assertIn("w8-core:chromium-desktop", text)
        self.assertIn("chromium-runtime-entrypoint", text)
        self.assertIn("agent_browser: ../agent-browser", text)
        self.assertNotIn("OPENAI_", text)
        self.assertNotIn("AI_GATEWAY_", text)
        self.assertNotIn("AGENT_BROWSER_MAX_STEPS", text)

    def test_agent_browser_package_is_model_agnostic(self) -> None:
        text = (ROOT.parent / "agent-browser/package.json").read_text()
        self.assertIn('"agent-browser": "0.27.3"', text)
        self.assertNotIn('"openai"', text)
        self.assertNotIn('"anthropic"', text)
        self.assertNotIn('"gemini"', text)

    def test_local_script_uses_standard_ports(self) -> None:
        text = (ROOT / "scripts/run_local.sh").read_text()
        self.assertIn("CDP_PORT:-9222", text)
        self.assertIn("VNC_PORT:-5900", text)
        self.assertIn("NOVNC_PORT:-6080", text)
        self.assertNotIn("39224", text)
        self.assertIn("CDP_HOST_PORT", text)

    def test_browser_payload_present(self) -> None:
        chrome = ROOT / "build/chrome"
        self.assertTrue(chrome.exists(), "build/chrome")
        self.assertTrue(chrome.stat().st_mode & 0o111, "build/chrome should be executable")


if __name__ == "__main__":
    unittest.main()
