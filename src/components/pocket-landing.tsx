"use client";

import Link from "next/link";
import PocketGL from "./pocket-gl";
import { ArrowRight, AudioLines, Library, Mic, SlidersHorizontal } from "lucide-react";
import styles from "./pocket-landing.module.css";

type PocketLandingProps = {
  authenticated: boolean;
  onLogin: () => void;
};

const steps = [
  { icon: Library, title: "Create a voice agent", detail: "Give it instructions, a first greeting, and a voice in your dashboard." },
  { icon: SlidersHorizontal, title: "Build your Stick library", detail: "Add the agents you want to carry, then choose between them on the Stick." },
  { icon: Mic, title: "Hold A to talk", detail: "Choose an agent over Wi-Fi. Hold the front button to speak and release it to listen." },
];

export default function PocketLanding({ authenticated, onLogin }: PocketLandingProps) {
  const action = (label: string, className: string) => {
    const content = <>{label}<ArrowRight size={17} aria-hidden="true" /></>;
    return authenticated
      ? <Link href="/dashboard" className={className}>{content}</Link>
      : <button type="button" onClick={onLogin} className={className}>{content}</button>;
  };

  return (
    <div className={styles.page}>
      <header className={styles.header}>
        <div className={styles.brand}><span className={styles.mark}><AudioLines size={23} aria-hidden="true" /></span>PocketFullOfAgents</div>
        {action(authenticated ? "Dashboard" : "Log in", styles.navAction)}
      </header>
      <main className={styles.main}>
        <section className={styles.hero} aria-labelledby="pocket-heading">
          <div className={styles.copy}>
            <h1 id="pocket-heading">Your agents,<br />in your <em>pocket.</em></h1>
            <p>Give your voice agents a button. Add them to your M5StickS3, pick one, and hold A to talk over Wi-Fi.</p>
            {action(authenticated ? "Open dashboard" : "Get started", styles.primary)}
          </div>
          <figure className={styles.figure}>
            <PocketGL />
            {/* eslint-disable-next-line @next/next/no-img-element */}
            <img src="/stick-pocket.webp" alt="M5StickS3 pocket voice device" fetchPriority="high" className={styles.photo} />
            <figcaption className={styles.caption}>M5StickS3</figcaption>
          </figure>
        </section>
        <section className={styles.guide} aria-labelledby="pocket-guide">
          <h2 id="pocket-guide">How it works</h2>
          <ol className={styles.steps}>
            {steps.map(({ icon: Icon, title, detail }) => (
              <li key={title}>
                <Icon size={23} strokeWidth={1.6} aria-hidden="true" />
                <h3>{title}</h3>
                <p>{detail}</p>
              </li>
            ))}
          </ol>
        </section>
      </main>
    </div>
  );
}
