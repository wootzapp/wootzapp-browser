"""Local browser-only regression checks; no model or benchmark calls.

Capture the old build with --baseline, then run the new build with --before
pointing at those artifacts. Browser endpoints must belong to disposable tests.

Visible-range extraction covers clipped inline-rich <p> paragraphs in the main
document. Other inline-rich blocks keep their first-500-characters behaviour.
Frame, shadow, hidden and padded-content behaviour is outside this change.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
from pathlib import Path
import re
import unittest

import aiohttp


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/third_party/blink/renderer/core/inspector/inspector_chromiumrl_agent.cc"
FIXTURE = Path(__file__).parent / "fixtures/overlay.html"
UNCHANGED = (
    "forms", "tables", "lists", "iframes", "shadow", "hidden", "padded",
    "short_inline", "long_leaf", "long_div", "complete_paragraph",
)
POSITIONS = ("top", "middle", "bottom", "partly_visible")


def normalized(value):
    return re.sub(r"\s+", " ", value).strip()


def projection(model):
    return "\n\n".join("\n".join(s["lines"]) for s in model["sections"]) + "\n"


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
        await self.call("Page.navigate", {"url": "about:blank"}, session)
        frame = (await self.call("Page.getFrameTree", session=session))["frameTree"]["frame"]["id"]
        await self.call("Page.setDocumentContent", {
            "frameId": frame, "html": FIXTURE.read_text().replace("__FIXTURE__", name),
        }, session)
        await self.settle(session)
        if not await self.evaluate(session, "window.fixtureReady === true"):
            raise AssertionError("Fixture did not initialize")

    async def capture(self, session, *, offscreen=False, max_text=200000):
        snapshot = (await self.call("ChromiumRL.captureStructuredSnapshot", {
            "inViewportOnly": not offscreen, "includeOffscreen": offscreen,
            "maxNodes": 7000, "maxTextChars": max_text,
            "includeDiff": False, "updateBaseline": False,
        }, session))["snapshot"]
        model = (await self.call("ChromiumRL.getModelDOM", {"snapshot": snapshot}, session))["modelDOM"]
        screenshot = await self.call("Page.captureScreenshot", {
            "format": "png", "fromSurface": True, "captureBeyondViewport": False,
        }, session)
        return snapshot, model, base64.b64decode(screenshot["data"])


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
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    results = {"baseline": args.baseline, "fixtures": [], "u2": [], "errors": []}
    async with CDP(args.cdp_url) as cdp:
        results["browser"] = cdp.version
        target = (await cdp.call("Target.createTarget", {"url": "about:blank"}))["targetId"]
        session = await cdp.attach(target)
        await cdp.call("Emulation.setDeviceMetricsOverride", {
            "width": 1000, "height": 720, "deviceScaleFactor": 1, "mobile": False,
        }, session)

        async def save(name, offscreen=False, max_text=200000):
            snapshot, model, png = await cdp.capture(session, offscreen=offscreen, max_text=max_text)
            text = projection(model)
            (output / f"{name}.json").write_text(json.dumps(snapshot, ensure_ascii=False, indent=2) + "\n")
            (output / f"{name}.txt").write_text(text)
            (output / f"{name}.png").write_bytes(png)
            return snapshot, text, png

        for name in UNCHANGED:
            await cdp.page(session, name)
            for position in ([0, 1200] if name in {"long_leaf", "long_div", "hidden", "padded"} else [0]):
                await cdp.evaluate(session, f"scrollTo(0, {position})")
                await cdp.settle(session)
                key = name + f"_{position}"
                snapshot, text, png = await save(key)
                row = {"fixture": key, "sha256": hashlib.sha256(text.encode()).hexdigest()}
                if args.before:
                    old = (args.before / f"{key}.txt").read_bytes()
                    row["live_projection_equal"] = old == text.encode()
                    frozen = json.loads((args.before / f"{key}.json").read_text())
                    replay = projection((await cdp.call("ChromiumRL.getModelDOM", {"snapshot": frozen}, session))["modelDOM"])
                    row["frozen_projection_equal"] = old == replay.encode()
                    if not row["live_projection_equal"] or not row["frozen_projection_equal"]:
                        results["errors"].append(f"Projection changed: {key}")
                results["fixtures"].append(row)

        for position in POSITIONS:
            await cdp.page(session, "inline_paragraph")
            await cdp.evaluate(session, """(() => {
              const p = document.querySelector('#rich');
              const y = {top:p.offsetTop, middle:p.offsetTop+40*24,
                bottom:p.offsetTop+p.offsetHeight-innerHeight,
                partly_visible:p.offsetTop-innerHeight+96};
              scrollTo(0, y['""" + position + """']);
            })()""")
            await cdp.settle(session)
            snapshot, text, png = await save("u2_" + position)
            expected = await cdp.evaluate(session, """(() => {
              const p=document.querySelector('#rich'), walker=document.createTreeWalker(p, NodeFilter.SHOW_TEXT);
              const parts=[]; let n;
              while(n=walker.nextNode()) {
                let piece='';
                for(let i=0;i<n.length;i++) {
                  const r=document.createRange(); r.setStart(n,i);r.setEnd(n,i+1);
                  const visible=Array.from(r.getClientRects()).some(b=>
                    b.width>0 && b.height>0 && b.bottom>0 && b.top<innerHeight && b.right>0 && b.left<innerWidth);
                  if(visible) piece+=n.data[i];
                }
                if(piece) parts.push(piece);
              }
              return parts.join(' ');
            })()""")
            expected = normalized(expected)[:500]
            paragraph = next(n for n in snapshot["nodes"] if n["tag"] == "p")
            actual = paragraph.get("subtreeText", "")
            row = {"position": position, "expected": expected, "actual": actual,
                   "length": len(actual), "capture_correct": actual == expected,
                   "projection_contains_visible_text": expected.rstrip() in text}
            if args.before:
                row["screenshot_equal"] = (args.before / f"u2_{position}.png").read_bytes() == png
                if not row["screenshot_equal"]:
                    results["errors"].append(f"U2 screenshot changed: {position}")
            results["u2"].append(row)
            if actual != expected or len(actual) > 500 or expected.rstrip() not in text:
                results["errors"].append(f"U2 text mismatch: {position}")

        # Offscreen capture keeps the original prefix contract for rich paragraphs.
        await cdp.page(session, "inline_paragraph")
        await cdp.evaluate(session, "scrollTo(0, 1960)")
        await cdp.settle(session)
        _, text, _ = await save("inline_offscreen", offscreen=True)
        if args.before and (args.before / "inline_offscreen.txt").read_bytes() != text.encode():
            results["errors"].append("Non-viewport projection changed")

        results["teardown"] = await teardown_check(cdp, args.iterations)
        if not results["teardown"]["passed"]:
            results["errors"].append("Teardown regression failed")
        elif args.close_browser:
            target = (await cdp.call("Target.createTarget", {"url": "about:blank"}))["targetId"]
            session = await cdp.attach(target)
            await cdp.page(session, "iframes")
            await cdp.call("Browser.close")
            results["browser_close_accepted"] = True
    results["passed"] = not results["errors"]
    (output / "results.json").write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({k:v for k,v in results.items() if k not in {"browser", "u2", "fixtures"}}, indent=2))
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cdp-url", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--before", type=Path)
    parser.add_argument("--iterations", type=int, default=30)
    parser.add_argument("--close-browser", action="store_true",
                        help="Finish with Browser.close while a session and iframe are attached")
    raise SystemExit(asyncio.run(run(parser.parse_args())))
