// Stand-in for @mediapipe/tasks-vision, loaded by test_web_lifecycle under
// node. Creation is asynchronous like the real thing; the counters let the
// test see which tasks were made and which were released.
const F = (globalThis.__nghFake = { created: 0, closed: 0, failGpu: true });

class FakeTask {
  constructor() {
    F.created++;
  }
  close() {
    F.closed++;
  }
  detectForVideo() {
    return { faceLandmarks: [], landmarks: [] };
  }
}

async function createFromOptions(fileset, options) {
  await new Promise((resolve) => setTimeout(resolve, 20));
  if (F.failGpu && options.baseOptions.delegate === "GPU") {
    throw new Error("fake: no GPU");
  }
  return new FakeTask();
}

export const FilesetResolver = { forVisionTasks: async () => ({}) };
export const FaceLandmarker = { createFromOptions };
export const PoseLandmarker = { createFromOptions };
