import type { MeasurementSeries, MeasurementSummary } from "./measurementHistoryApi";

type InitialPlaybackLoadOptions = {
  loadPrefix: () => Promise<MeasurementSeries>;
  loadSummary: () => Promise<MeasurementSummary>;
  scheduleSummary: (load: () => void) => void;
  onPrefix: (series: MeasurementSeries) => void;
  onSummary: (summary: MeasurementSummary) => void;
  onPrefixError: (error: unknown) => void;
  onSummaryError: (error: unknown) => void;
};

/**
 * 首段曲线是首屏的唯一前置请求。统计请求只有在首段已经提交后才会调度，
 * 因此统计聚合、JSON解析或失败都不会阻塞曲线进入可显示状态。
 */
export const loadInitialPlayback = async ({
  loadPrefix,
  loadSummary,
  scheduleSummary,
  onPrefix,
  onSummary,
  onPrefixError,
  onSummaryError,
}: InitialPlaybackLoadOptions): Promise<void> => {
  let prefix: MeasurementSeries;
  try {
    prefix = await loadPrefix();
  } catch (error) {
    onPrefixError(error);
    return;
  }

  onPrefix(prefix);
  scheduleSummary(() => {
    void loadSummary().then(onSummary).catch(onSummaryError);
  });
};
