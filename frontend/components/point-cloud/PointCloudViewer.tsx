"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";

import { PCV1_MAX_POINTS } from "./cloudPreviewProtocol";
import type { CloudPreviewFrame } from "./cloudPreviewProtocol";
import { useCloudPreviewSocket } from "./useCloudPreviewSocket";

type ViewerResources = {
  renderer: THREE.WebGLRenderer;
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  controls: OrbitControls;
  geometry: THREE.BufferGeometry;
  positionAttribute: THREE.BufferAttribute;
  requestRender: () => void;
  fitView: () => void;
  resize: () => void;
};

function makeAxisLabel(text: string, color: string): THREE.Sprite {
  const canvas = document.createElement("canvas");
  canvas.width = 192;
  canvas.height = 80;
  const context = canvas.getContext("2d");
  if (!context) {
    throw new Error("浏览器无法创建东北天坐标轴标签");
  }
  context.font = "700 36px sans-serif";
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.lineWidth = 8;
  context.strokeStyle = "rgba(255, 255, 255, 0.95)";
  context.strokeText(text, canvas.width / 2, canvas.height / 2);
  context.fillStyle = color;
  context.fillText(text, canvas.width / 2, canvas.height / 2);

  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  const material = new THREE.SpriteMaterial({
    map: texture,
    depthTest: false,
    transparent: true,
  });
  const sprite = new THREE.Sprite(material);
  sprite.scale.set(1.2, 0.5, 1);
  sprite.renderOrder = 10;
  return sprite;
}

export default function PointCloudViewer({
  socketPath = "/ws/v1/cloud-preview",
  liveLabel = "实时点云",
  axisMode = "enu",
}: {
  socketPath?: string;
  liveLabel?: string;
  axisMode?: "enu" | "sensor";
} = {}) {
  const hostRef = useRef<HTMLDivElement>(null);
  const resourcesRef = useRef<ViewerResources | null>(null);
  const firstFrameRef = useRef(true);
  const [webglError, setWebglError] = useState<string | null>(null);

  const handleFrame = useCallback((frame: CloudPreviewFrame) => {
    const resources = resourcesRef.current;
    if (!resources) return;

    const target = resources.positionAttribute.array as Float32Array;
    target.set(frame.positions, 0);
    resources.positionAttribute.needsUpdate = true;
    resources.geometry.setDrawRange(0, frame.pointCount);
    resources.geometry.computeBoundingBox();
    resources.geometry.computeBoundingSphere();

    if (firstFrameRef.current) {
      firstFrameRef.current = false;
      resources.fitView();
    }
    resources.requestRender();
  }, []);

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;

    let animationFrame: number | null = null;
    let disposed = false;

    try {
      const renderer = new THREE.WebGLRenderer({
        antialias: true,
        alpha: true,
        powerPreference: "high-performance",
      });
      renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
      renderer.outputColorSpace = THREE.SRGBColorSpace;
      renderer.domElement.className = "cloud-canvas";
      renderer.domElement.setAttribute("aria-label", "实时点云三维预览");
      renderer.domElement.tabIndex = 0;
      host.appendChild(renderer.domElement);

      const scene = new THREE.Scene();
      scene.background = new THREE.Color(0xf5f8fc);
      scene.fog = new THREE.FogExp2(0xf5f8fc, 0.01);
      const enuSceneRoot = new THREE.Group();
      enuSceneRoot.name = "lidar-local-enu";
      scene.add(enuSceneRoot);

      const camera = new THREE.PerspectiveCamera(48, 1, 0.01, 2_000);
      camera.up.set(0, 0, 1);
      camera.position.set(8, -12, 12);

      const positions = new Float32Array(PCV1_MAX_POINTS * 3);
      const positionAttribute = new THREE.BufferAttribute(positions, 3);
      positionAttribute.setUsage(THREE.DynamicDrawUsage);
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute("position", positionAttribute);
      geometry.setDrawRange(0, 0);

      const material = new THREE.PointsMaterial({
        color: 0x1769ee,
        size: 0.045,
        sizeAttenuation: true,
        transparent: true,
        opacity: 0.95,
      });
      const points = new THREE.Points(geometry, material);
      enuSceneRoot.add(points);

      const grid = new THREE.GridHelper(100, 100, 0x9fb8d8, 0xd8e2ef);
      grid.rotation.x = Math.PI / 2;
      grid.material.transparent = true;
      grid.material.opacity = 0.68;
      enuSceneRoot.add(grid);

      const axes = new THREE.AxesHelper(2.0);
      enuSceneRoot.add(axes);
      const eastLabel = makeAxisLabel(axisMode === "enu" ? "东 E" : "X", "#d43e50");
      eastLabel.position.set(2.45, 0, 0);
      const northLabel = makeAxisLabel(axisMode === "enu" ? "北 N" : "Y", "#14946b");
      northLabel.position.set(0, 2.45, 0);
      const upLabel = makeAxisLabel(axisMode === "enu" ? "天 U" : "Z", "#1769ee");
      upLabel.position.set(0, 0, 2.45);
      enuSceneRoot.add(eastLabel, northLabel, upLabel);

      const controls = new OrbitControls(camera, renderer.domElement);
      controls.enableDamping = false;
      controls.screenSpacePanning = true;
      controls.target.set(3, 1, 1);

      const render = () => {
        animationFrame = null;
        if (!disposed) renderer.render(scene, camera);
      };
      const requestRender = () => {
        if (animationFrame === null) {
          animationFrame = window.requestAnimationFrame(render);
        }
      };

      const resize = () => {
        const width = Math.max(host.clientWidth, 1);
        const height = Math.max(host.clientHeight, 1);
        renderer.setSize(width, height, false);
        camera.aspect = width / height;
        camera.updateProjectionMatrix();
        requestRender();
      };

      const fitView = () => {
        const box = geometry.boundingBox;
        if (!box || box.isEmpty()) {
          requestRender();
          return;
        }

        const center = box.getCenter(new THREE.Vector3());
        const size = box.getSize(new THREE.Vector3());
        const span = Math.max(size.x, size.y, size.z, 2);
        const distance = span * 1.45;
        camera.position.set(
          center.x + distance * 0.75,
          center.y - distance,
          center.z + distance,
        );
        controls.target.copy(center);
        controls.update();
        requestRender();
      };

      controls.addEventListener("change", requestRender);
      const resizeObserver = new ResizeObserver(resize);
      resizeObserver.observe(host);

      resourcesRef.current = {
        renderer,
        scene,
        camera,
        controls,
        geometry,
        positionAttribute,
        requestRender,
        fitView,
        resize,
      };
      resize();

      return () => {
        disposed = true;
        resizeObserver.disconnect();
        controls.removeEventListener("change", requestRender);
        controls.dispose();
        if (animationFrame !== null) window.cancelAnimationFrame(animationFrame);
        geometry.dispose();
        material.dispose();
        axes.dispose();
        for (const label of [eastLabel, northLabel, upLabel]) {
          const labelMaterial = label.material as THREE.SpriteMaterial;
          labelMaterial.map?.dispose();
          labelMaterial.dispose();
        }
        renderer.dispose();
        renderer.forceContextLoss();
        renderer.domElement.remove();
        resourcesRef.current = null;
      };
    } catch (error) {
      const detail = error instanceof Error
        ? error.message
        : "浏览器无法创建WebGL上下文";
      queueMicrotask(() => {
        if (!disposed) setWebglError(detail);
      });
    }
  }, [axisMode]);

  const connection = useCloudPreviewSocket(handleFrame, socketPath);
  const isStreaming = connection.streamState === "streaming" && !webglError;
  const statusText = webglError ?? connection.detail;
  const showStatusOverlay = webglError !== null
    || connection.connection === "connecting"
    || connection.connection === "reconnecting"
    || connection.streamState === "degraded"
    || connection.streamState === "ros_unavailable";

  return (
    <div className="cloud-stage dashboard-cloud-stage cloud-stage--3d">
      <div ref={hostRef} className="cloud-canvas-host" />
      <div className={`cloud-live-badge cloud-live-badge--${connection.streamState}`}>
        <i />
        {isStreaming ? liveLabel : "预览状态"}
      </div>
      {showStatusOverlay && (
        <div className="cloud-overlay-status" role="status">
          <span>⁙</span>
          <strong>{statusText}</strong>
          <small>
            {connection.connection === "reconnecting"
              ? "正在自动恢复连接"
              : "核心采集与测量任务不受预览状态影响"}
          </small>
        </div>
      )}
    </div>
  );
}
