"""Local target, session, iframe and popup teardown regression checks.

Browser endpoints must belong to disposable tests.
"""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import re
import unittest

import aiohttp


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/third_party/blink/renderer/core/inspector/inspector_chromiumrl_agent.cc"


class CDP:
    def __init__(self, endpoint):
        self.endpoint = endpoint.rstrip("/")
        self.sequence = 0
        self.crashes = []

    async def __aenter__(self):
        self.http = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=30))
        async with self.http.get(self.endpoint + "/json/version") as response:
            response.raise_for_status()
            self.version = await response.json()
        self.ws = await self.http.ws_connect(self.version["webSocketDebuggerUrl"])
        return self

    async def __aexit__(self, *args):
        await self.ws.close()
        await self.http.close()

    async def call(self, method, params=None, session=None):
        self.sequence += 1
        request = {"id": self.sequence, "method": method, "params": params or {}}
        if session:
            request["sessionId"] = session
        await self.ws.send_json(request)
        async with asyncio.timeout(30):
            while True:
                message = await self.ws.receive_json()
                if "crash" in message.get("method", "").lower():
                    self.crashes.append(message)
                if message.get("id") != request["id"]:
                    continue
                if "error" in message:
                    raise RuntimeError(f"{method}: {message['error']}")
                return message.get("result", {})

    async def attach(self, target):
        response = await self.call("Target.attachToTarget", {
            "targetId": target, "flatten": True,
        })
        session = response["sessionId"]
        await self.call("Page.enable", session=session)
        await self.call("ChromiumRL.enable", session=session)
        return session

    async def evaluate(self, session, expression):
        response = await self.call("Runtime.evaluate", {
            "expression": expression, "returnByValue": True, "awaitPromise": True,
        }, session)
        if "exceptionDetails" in response:
            raise RuntimeError(str(response["exceptionDetails"]))
        return response["result"].get("value")

    async def settle(self, session):
        await self.evaluate(session, "new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)))")

    async def page(self, session, name):
        fixtures = {
            "forms": '<label>Account <input value="Ada"></label><button>Save</button>',
            "iframes": '<iframe srcdoc="<p>Child frame</p>"></iframe>',
        }
        await self.call("Page.navigate", {"url": "about:blank"}, session)
        frame = (await self.call("Page.getFrameTree", session=session))["frameTree"]["frame"]["id"]
        await self.call("Page.setDocumentContent", {
            "frameId": frame, "html": fixtures[name],
        }, session)
        await self.settle(session)


class SourceRegressionTests(unittest.TestCase):
    def test_disposal_precedes_gc_destructor(self):
        source = SOURCE.read_text()
        destructor = re.search(r"InspectorChromiumRLAgent::~InspectorChromiumRLAgent\(\)(.*?)(?=\n\n)", source, re.S)
        self.assertIsNotNone(destructor)
        self.assertNotIn("UnregisterAgent", destructor.group())
        dispose = re.search(r"void InspectorChromiumRLAgent::Dispose\(\)\s*\{(.*?)\n\}", source, re.S)
        self.assertIsNotNone(dispose)
        self.assertIn("UnregisterAgent();", dispose.group())
        self.assertIn("InspectorBaseAgent", dispose.group())

    def test_unregister_preserves_other_sessions_registration(self):
        source = SOURCE.read_text()
        method = re.search(r"void InspectorChromiumRLAgent::UnregisterAgent\(\)\s*\{(.*?)\n\}", source, re.S)
        self.assertIsNotNone(method)
        self.assertIn("== this", method.group())


async def teardown_check(cdp, iterations):
    result = {"iterations_requested": iterations, "iterations_completed": 0, "error": None}
    try:
        for number in range(iterations):
            target = (await cdp.call("Target.createTarget", {"url": "about:blank"}))["targetId"]
            first = await cdp.attach(target)
            second = await cdp.attach(target)
            await cdp.page(second, "iframes")
            await cdp.evaluate(second, "document.querySelector('iframe').src = 'about:blank'")
            await cdp.settle(second)
            await cdp.evaluate(second, "document.querySelector('iframe').remove()")
            await cdp.call("Target.detachFromTarget", {"sessionId": first})
            # Finalize the detached session while another session still owns this frame.
            await cdp.call("HeapProfiler.collectGarbage", session=second)
            await cdp.call("ChromiumRL.captureStructuredSnapshot", {}, second)
            before = {t["targetId"] for t in (await cdp.call("Target.getTargets"))["targetInfos"]}
            await cdp.call("Runtime.evaluate", {
                "expression": f"window.open('about:blank', 'popup_{number}') !== null",
                "returnByValue": True, "userGesture": True,
            }, second)
            infos = (await cdp.call("Target.getTargets"))["targetInfos"]
            popups = [t["targetId"] for t in infos if t["targetId"] not in before and t.get("openerId") == target]
            if not popups:
                raise AssertionError("Popup was not created")
            for popup in popups:
                popup_session = await cdp.attach(popup)
                await cdp.page(popup_session, "forms")
                if number % 2:
                    await cdp.call("Target.closeTarget", {"targetId": target})
                await cdp.call("Target.closeTarget", {"targetId": popup})
            if not number % 2:
                await cdp.call("Target.closeTarget", {"targetId": target})
            result["iterations_completed"] += 1
            if cdp.crashes:
                raise AssertionError(f"Target crash events: {cdp.crashes}")
        await cdp.call("Browser.getVersion")
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    result["crashes"] = cdp.crashes
    result["passed"] = result["error"] is None and not cdp.crashes
    return result


async def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    async with CDP(args.cdp_url) as cdp:
        result = await teardown_check(cdp, args.iterations)
        if result["passed"] and args.close_browser:
            target = (await cdp.call("Target.createTarget", {"url": "about:blank"}))["targetId"]
            session = await cdp.attach(target)
            await cdp.page(session, "iframes")
            await cdp.call("Browser.close")
            result["browser_close_accepted"] = True
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cdp-url", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--iterations", type=int, default=30)
    parser.add_argument("--close-browser", action="store_true",
                        help="Finish with Browser.close while a session and iframe are attached")
    raise SystemExit(asyncio.run(run(parser.parse_args())))
