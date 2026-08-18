import assert from "node:assert/strict";
import test from "node:test";

const {
  createPlaybackSeriesCache,
  isSeriesWindowCached,
  mergeSeriesIntoCache,
} = await import("../components/playback/playbackSeriesCache.ts");

const series = ({ start, end, downsampled, samples, sourceCount = samples.length }) => ({
  taskId: "task-1",
  domainStartTimestampMs: 0,
  domainEndTimestampMs: 10000,
  requestedStartTimestampMs: start,
  requestedEndTimestampMs: end,
  sourceSampleCount: sourceCount,
  returnedSampleCount: samples.length,
  downsampled,
  samples,
});

const sample = (sampleIndex, timestampMs, heightM = 4.5) => ({
  sampleIndex,
  timestampMs,
  elapsedMs: timestampMs,
  heightM,
  valid: true,
});

test("playback cache merges prior and newly loaded windows by sample index", () => {
  const cache = createPlaybackSeriesCache();
  const first = mergeSeriesIntoCache(cache, series({
    start: 0,
    end: 1000,
    downsampled: false,
    samples: [sample(0, 0), sample(1, 500), sample(2, 1000)],
  }));
  assert.deepEqual(first.samples.map((item) => item.sampleIndex), [0, 1, 2]);

  const second = mergeSeriesIntoCache(cache, series({
    start: 1000,
    end: 2000,
    downsampled: false,
    samples: [sample(2, 1000, 4.4), sample(3, 1500), sample(4, 2000)],
  }));
  assert.deepEqual(second.samples.map((item) => item.sampleIndex), [0, 1, 2, 3, 4]);
  assert.equal(second.samples.find((item) => item.sampleIndex === 2)?.heightM, 4.4);
  assert.equal(isSeriesWindowCached(cache, 0, 900, 6000), true);
  assert.equal(isSeriesWindowCached(cache, 1100, 1900, 6000), true);
  assert.equal(isSeriesWindowCached(cache, 2100, 2500, 6000), false);
});

test("downsampled cache is reused at equal/coarser resolution but refreshed when zoom requires detail", () => {
  const cache = createPlaybackSeriesCache();
  const points = Array.from({ length: 101 }, (_, index) => sample(index, index * 10));
  mergeSeriesIntoCache(cache, series({
    start: 0,
    end: 1000,
    downsampled: true,
    samples: points,
    sourceCount: 1000,
  }));
  assert.equal(isSeriesWindowCached(cache, 0, 1000, 100), true);
  assert.equal(isSeriesWindowCached(cache, 400, 600, 6000), false);
});
