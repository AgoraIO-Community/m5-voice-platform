import type { Metadata } from "next";
import { ThemeSync } from "@/components/theme-sync";
import "./globals.css";

export const metadata: Metadata = {
  title: "PocketFullOfAgents",
  description:
    "Create voice agents, choose one for your M5StickS3, and talk to it over Wi-Fi.",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body className="bg-background text-foreground min-h-screen antialiased">
        <ThemeSync />
        {children}
      </body>
    </html>
  );
}
