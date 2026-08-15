# asyncio: the event loop, and hardware waking a coroutine.
#
# Needs no wiring. The interrupt in the last test comes from a virtual timer,
# which is the same delivery path a DMA-completion interrupt would use.
import time

import asyncio
import machine

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
        print("  PASS ", name)
    else:
        failed += 1
        print("  FAIL ", name)


order = []


async def ticker(name, n, ms):
    for _ in range(n):
        await asyncio.sleep_ms(ms)
        order.append(name)


async def nap(ms):
    await asyncio.sleep_ms(ms)


async def test_interleave():
    order.clear()
    await asyncio.gather(ticker("a", 4, 20), ticker("b", 4, 30))
    check("both tasks run to completion", order.count("a") == 4 and order.count("b") == 4)
    # Different periods, so a loop that really is scheduling will mix them.
    check("tasks interleave rather than serialise", order != ["a"] * 4 + ["b"] * 4)


async def test_gather_is_concurrent():
    t0 = time.ticks_ms()
    await asyncio.gather(nap(120), nap(120), nap(120))
    dt = time.ticks_diff(time.ticks_ms(), t0)
    # One sleep's worth, not three: the number is the whole point of the test.
    check("gather waits once, not once per task (%d ms)" % dt, 100 < dt < 250)


async def test_threadsafeflag():
    """An interrupt releasing a coroutine, with the loop still running.

    This is the shape any "start a transfer, await completion" driver takes:
    the handler calls set(), the coroutine sits in wait(), and everything else
    keeps being scheduled in between. ThreadSafeFlag is an io.IOBase polled
    through asyncio's IOQueue, so it is also what proves MICROPY_PY_SELECT is
    present -- without it this raises rather than waiting.
    """
    flag = asyncio.ThreadSafeFlag()
    counter = [0]

    async def busy():
        while True:
            counter[0] += 1
            await asyncio.sleep_ms(5)

    t = machine.Timer(-1, period=150, mode=machine.Timer.ONE_SHOT, callback=lambda _: flag.set())
    helper = asyncio.create_task(busy())
    t0 = time.ticks_ms()
    await flag.wait()
    dt = time.ticks_diff(time.ticks_ms(), t0)
    helper.cancel()
    t.deinit()

    check("ThreadSafeFlag wakes on an interrupt (%d ms)" % dt, 100 < dt < 400)
    check("the loop kept scheduling while it waited (%d)" % counter[0], counter[0] > 5)


async def test_cancel():
    async def forever():
        while True:
            await asyncio.sleep_ms(10)

    t = asyncio.create_task(forever())
    await asyncio.sleep_ms(30)
    t.cancel()
    await asyncio.sleep_ms(30)
    check("a cancelled task stops", t.done())


async def main():
    await test_interleave()
    await test_gather_is_concurrent()
    await test_threadsafeflag()
    await test_cancel()


print("asyncio")
asyncio.run(main())
print("%u passed, %u failed" % (passed, failed))
