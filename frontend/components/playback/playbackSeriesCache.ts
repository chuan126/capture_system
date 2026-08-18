import type { ClearanceSample } from "./InteractiveClearanceChart";
import type { MeasurementSeries } from "./measurementHistoryApi";

export type CachedSeriesRange = {
  startTimestampMs: number;
  endTimestampMs: number;
  resolutionMs: number;
};

export type PlaybackSeriesCache = {
  samplesByIndex: Map<number, ClearanceSample>;
  ranges: CachedSeriesRange[];
};

const rangeResolutionMs = (series: MeasurementSeries): number => {
  if (!series.downsampled) return 0;
  const span = Math.max(1, series.requestedEndTimestampMs - series.requestedStartTimestampMs);
  return span / Math.max(1, series.returnedSampleCount - 1);
};

export const createPlaybackSeriesCache = (): PlaybackSeriesCache => ({
  samplesByIndex: new Map<number, ClearanceSample>(),
  ranges: [],
});

export const mergeSeriesIntoCache = (
  cache: PlaybackSeriesCache,
  series: MeasurementSeries,
): MeasurementSeries => {
  series.samples.forEach((sample) => cache.samplesByIndex.set(sample.sampleIndex, sample));
  cache.ranges.push({
    startTimestampMs: series.requestedStartTimestampMs,
    endTimestampMs: series.requestedEndTimestampMs,
    resolutionMs: rangeResolutionMs(series),
  });
  const samples = [...cache.samplesByIndex.values()].sort((left, right) => (
    left.timestampMs - right.timestampMs || left.sampleIndex - right.sampleIndex
  ));
  return {
    ...series,
    samples,
    returnedSampleCount: samples.length,
  };
};

export const isSeriesWindowCached = (
  cache: PlaybackSeriesCache,
  startTimestampMs: number,
  endTimestampMs: number,
  maxPoints: number,
): boolean => {
  const start = Math.min(startTimestampMs, endTimestampMs);
  const end = Math.max(startTimestampMs, endTimestampMs);
  const requestedSpan = Math.max(1, end - start);
  const targetResolutionMs = requestedSpan / Math.max(1, maxPoints - 1);
  const adequate = cache.ranges
    .filter((range) => range.resolutionMs === 0 || range.resolutionMs <= targetResolutionMs * 1.05)
    .map((range) => ({
      start: Math.max(start, range.startTimestampMs),
      end: Math.min(end, range.endTimestampMs),
    }))
    .filter((range) => range.end >= range.start)
    .sort((left, right) => left.start - right.start || left.end - right.end);

  if (adequate.length === 0 || adequate[0].start > start) return false;
  let coveredUntil = adequate[0].end;
  if (coveredUntil >= end) return true;
  for (let index = 1; index < adequate.length; index += 1) {
    const range = adequate[index];
    if (range.start > coveredUntil + 1) return false;
    coveredUntil = Math.max(coveredUntil, range.end);
    if (coveredUntil >= end) return true;
  }
  return false;
};
