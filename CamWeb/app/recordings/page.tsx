import AppShell from "@/components/AppShell";
import Recordings from "@/components/Recordings";

export const metadata = { title: "Recordings - CamVision" };

export default function RecordingsPage() {
  return (
    <AppShell>
      <Recordings />
    </AppShell>
  );
}
