import type { NormalizedViewWindow } from "./clearanceChartViewport";
import type { MeasurementSeries } from "./measurementHistoryApi";

export type SeriesWindowRequest = {
  startTimestampMs: number;
  endTimestampMs: number;
};

export const getSeriesWindowRequest = (
  series: MeasurementSeries,
  viewWindow: NormalizedViewWindow,
): SeriesWindowRequest | null => {
  const fullSpanMs = Math.max(1, series.domainEndTimestampMs - series.domainStartTimestampMs);
  const visibleStart = Math.floor(series.domainStartTimestampMs + fullSpanMs * viewWindow.start);
  const visibleEnd = Math.ceil(series.domainStartTimestampMs + fullSpanMs * viewWindow.end);
  const visibleSpan = Math.max(1, visibleEnd - visibleStart);
  const margin = Math.max(40, Math.round(visibleSpan * 0.04));
  const requestedStart = Math.max(series.domainStartTimestampMs, visibleStart - margin);
  const requestedEnd = Math.min(series.domainEndTimestampMs, visibleEnd + margin);
  const currentSpan = Math.max(1, series.requestedEndTimestampMs - series.requestedStartTimestampMs);
  const requestedSpan = Math.max(1, requestedEnd - requestedStart);
  const currentHasFullResolution = !series.downsampled
    && series.requestedStartTimestampMs <= requestedStart
    && series.requestedEndTimestampMs >= requestedEnd;
  const currentDownsampledWindowMatches = series.downsampled
    && currentSpan <= requestedSpan * 1.15
    && series.requestedStartTimestampMs <= requestedStart
    && series.requestedEndTimestampMs >= requestedEnd;

  if (currentHasFullResolution || currentDownsampledWindowMatches) return null;
  return { startTimestampMs: requestedStart, endTimestampMs: requestedEnd };
};

export const getUserSeriesWindowRequest = (
  series: MeasurementSeries,
  viewWindow: NormalizedViewWindow,
  userViewRevision: number,
): SeriesWindowRequest | null => {
  if (userViewRevision <= 0) return null;
  return getSeriesWindowRequest(series, viewWindow);
};
