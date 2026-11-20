import type { ReactNode } from "react";

import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Textarea } from "@/components/ui/textarea";
import { cn } from "@/lib/utils";

const labels = {
  name: "Agent name",
  prompt: "Instructions",
  greeting: "First spoken message",
};

type Props = {
  kind: keyof typeof labels;
  id: string;
  value?: string;
  onChange?: (value: string) => void;
  className?: string;
  controlClassName?: string;
  children?: ReactNode;
};

export function AgentTextField({
  kind,
  id,
  value,
  onChange,
  className,
  controlClassName,
  children,
}: Props) {
  const Control = kind === "name" ? Input : Textarea;

  return (
    <div className={cn("space-y-2", className)}>
      <Label htmlFor={id}>{labels[kind]}</Label>
      <Control
        id={id}
        name={kind}
        value={value}
        onChange={(event) => onChange?.(event.target.value)}
        required={kind !== "greeting"}
        className={controlClassName}
      />
      {children}
    </div>
  );
}
