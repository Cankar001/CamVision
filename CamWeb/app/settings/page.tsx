import AppShell from "@/components/AppShell";
import ConnectionForm from "@/components/ConnectionForm";

export const metadata = { title: "Connection - CamVision" };

export default function SettingsPage() {
  return (
    <AppShell>
      <ConnectionForm />
    </AppShell>
  );
}
