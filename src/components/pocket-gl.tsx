"use client";

import { useEffect, useRef } from "react";
import styles from "./pocket-landing.module.css";

const vertex = `
attribute vec2 point;
varying float opacity;
uniform float time;
uniform vec2 cursor;
uniform float aspect;
void main() {
  float x = point.x;
  float layer = point.y;
  float spread = (layer - .5) * 2.;
  float phase = time * .65 + cursor.x * .6;
  float envelope = .35 + .65 * exp(-2.8 * x*x);
  float wave = sin(4.8*x + spread*2.6 + phase) * .23
             + sin(8.*x - phase*.75 + layer*3.) * .095;
  float y = (spread * .31 + wave * envelope + .19*x + cursor.y*.045) * aspect;
  opacity = .24 + .25 * sin(layer * 3.14159);
  gl_Position = vec4(x,y,0.,1.);
}`;
const fragment = `
precision mediump float;
varying float opacity;
void main() { gl_FragColor = vec4(.53,.28,.11,opacity); }`;

export default function PocketGL() {
  const ref = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = ref.current;
    const stage = canvas?.parentElement;
    if (!canvas || !stage) return;
    const gl = canvas.getContext("webgl", { alpha: true, premultipliedAlpha: false, depth: false });
    if (!gl) return;
    const program = gl.createProgram();
    if (!program) return;
    for (const [type, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]] as const) {
      const shader = gl.createShader(type);
      if (!shader) continue;
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      gl.attachShader(program, shader);
      gl.deleteShader(shader);
    }
    gl.linkProgram(program);
    const buffer = gl.createBuffer();
    if (!buffer || !gl.getProgramParameter(program, gl.LINK_STATUS)) {
      gl.deleteBuffer(buffer);
      gl.deleteProgram(program);
      return;
    }
    const lines = 33;
    const samples = 144;
    const points = new Float32Array(lines * samples * 2);
    for (let line = 0; line < lines; line++) {
      for (let i = 0; i < samples; i++) {
        const offset = 2 * (line * samples + i);
        points[offset] = 2 * i / (samples - 1) - 1;
        points[offset + 1] = line / (lines - 1);
      }
    }
    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, points, gl.STATIC_DRAW);
    const point = gl.getAttribLocation(program, "point");
    gl.enableVertexAttribArray(point);
    gl.vertexAttribPointer(point, 2, gl.FLOAT, false, 0, 0);
    const time = gl.getUniformLocation(program, "time");
    const cursor = gl.getUniformLocation(program, "cursor");
    const aspect = gl.getUniformLocation(program, "aspect");
    let x = 0, y = 0, elapsed = 0, previous = 0, frame = 0, visible = false;
    const motion = matchMedia("(prefers-reduced-motion: reduce)");
    const draw = () => {
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.uniform1f(aspect, canvas.width / canvas.height);
      gl.uniform1f(time, elapsed);
      gl.uniform2f(cursor, x, y);
      for (let line = 0; line < lines; line++) gl.drawArrays(gl.LINE_STRIP, line * samples, samples);
    };
    const tick = (now: number) => {
      if (previous) elapsed += Math.min(now - previous, 80) / 1000;
      previous = now;
      draw();
      frame = requestAnimationFrame(tick);
    };
    const sync = () => {
      cancelAnimationFrame(frame);
      previous = 0;
      if (document.hidden || !visible) return;
      draw();
      if (!motion.matches) frame = requestAnimationFrame(tick);
    };
    const resize = () => {
      const dpr = Math.min(devicePixelRatio || 1, 1.5);
      canvas.width = Math.max(1, Math.round(stage.clientWidth * dpr));
      canvas.height = Math.max(1, Math.round(stage.clientHeight * dpr));
      gl.viewport(0, 0, canvas.width, canvas.height);
      sync();
    };
    const move = (event: PointerEvent) => {
      if (motion.matches) return;
      const rect = stage.getBoundingClientRect();
      x = 2 * (event.clientX - rect.left) / rect.width - 1;
      y = 1 - 2 * (event.clientY - rect.top) / rect.height;
    };
    const reset = () => { x = 0; y = 0; };
    const observer = new IntersectionObserver(([entry]) => { visible = entry.isIntersecting; sync(); });
    const size = new ResizeObserver(resize);
    observer.observe(stage);
    size.observe(stage);
    stage.addEventListener("pointermove", move);
    stage.addEventListener("pointerleave", reset);
    motion.addEventListener("change", sync);
    document.addEventListener("visibilitychange", sync);
    resize();
    return () => {
      cancelAnimationFrame(frame);
      size.disconnect();
      observer.disconnect();
      stage.removeEventListener("pointermove", move);
      stage.removeEventListener("pointerleave", reset);
      motion.removeEventListener("change", sync);
      document.removeEventListener("visibilitychange", sync);
      gl.deleteProgram(program);
      gl.deleteBuffer(buffer);
    };
  }, []);

  return <canvas ref={ref} className={styles.field} aria-hidden="true" />;
}
