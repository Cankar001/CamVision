import AppShell from "@/components/AppShell";
import Dashboard from "@/components/Dashboard";

export const metadata = { title: "Overview - CamVision" };

export default function DashboardPage() {
  return (
    <AppShell>
      <Dashboard />
    </AppShell>
  );
}
