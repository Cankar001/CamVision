"use client";

import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";
import { useEffect } from "react";
import { endSession, useSession } from "@/lib/auth";
import { getBackend } from "@/lib/backend";
import { useHydrated } from "@/lib/store";
import styles from "./AppShell.module.css";
import ui from "./ui.module.css";

const NAV = [
  { href: "/dashboard", label: "Overview" },
  { href: "/recordings", label: "Recordings" },
  { href: "/settings", label: "Connection" },
];

/** The frame of all pages behind the login: the navigation, and the guard, which sends everybody without a session to the login screen. */
export default function AppShell({ children }: { children: React.ReactNode }) {
  const router = useRouter();
  const pathname = usePathname();
  const hydrated = useHydrated();
  const { user } = useSession();

  useEffect(() => {
    if (hydrated && !user) {
      router.replace("/");
    }
  }, [hydrated, user, router]);

  if (!hydrated || !user) {
    return <div className={styles.loading}>Loading...</div>;
  }

  return (
    <div className={styles.shell}>
      <header className={styles.header}>
        <div className={styles.headerInner}>
          <span className={styles.brand}>CamVision</span>
          <nav className={styles.nav}>
            {NAV.map((item) => (
              <Link key={item.href} href={item.href} className={`${styles.link} ${pathname === item.href ? styles.active : ""}`}>
                {item.label}
              </Link>
            ))}
          </nav>
          <div className={styles.user}>
            <span>{user}</span>
            <button
              className={`${ui.button} ${ui.secondary}`}
              onClick={() => {
                // The cookie is ended by the backend, whatever the answer is the session here ends too.
                getBackend()
                  .logout()
                  .catch(() => {})
                  .finally(() => {
                    endSession();
                    router.replace("/");
                  });
              }}
            >
              Log out
            </button>
          </div>
        </div>
      </header>
      <main className={styles.main}>{children}</main>
    </div>
  );
}
