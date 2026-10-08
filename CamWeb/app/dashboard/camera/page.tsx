import { Suspense } from "react";
import AppShell from "@/components/AppShell";
import CameraDetail from "@/components/CameraDetail";

export const metadata = { title: "Camera - CamVision" };

export default function CameraPage() {
  return (
    <AppShell>
      {/* The camera comes from the address (?name=...), which is only known in the browser. */}
      <Suspense fallback={null}>
        <CameraDetail />
      </Suspense>
    </AppShell>
  );
}
