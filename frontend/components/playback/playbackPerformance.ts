import { DEVTOOLS_ENABLED } from "@/components/devtools/devtoolsEntry.generated";

const selectedAtByTask = new Map<string, number>();

export const markPlaybackTiming = (
  taskId: string,
  stage: string,
  detail?: Record<string, number | string | boolean>,
): void => {
  if (!DEVTOOLS_ENABLED || typeof performance === "undefined") return;

  const now = performance.now();
  if (stage === "task selected") selectedAtByTask.set(taskId, now);
  const selectedAt = selectedAtByTask.get(taskId) ?? now;
  console.debug("[playback timing]", {
    taskId,
    stage,
    elapsedSinceSelectionMs: Number((now - selectedAt).toFixed(2)),
    ...detail,
  });
};
