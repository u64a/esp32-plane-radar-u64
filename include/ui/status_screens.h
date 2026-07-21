#pragma once

#include <cstdint>

#include "core/factory_erase.h"
#include "core/provision_button.h"

// Functional status screens for boot, the secure setup portal, the credential
// transaction, the button gesture, and factory erase. Layout is intentionally
// plain (single framebuffer, no second buffer); Dallas polishes it after review.

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();

/** Bringing up the secure setup radio + secrets (no listener yet). */
void statusScreenPortalPreparing();

/**
 * Secured setup portal is live. Shows the MAC-derived SSID, the one-time WPA2
 * password (this panel is the ONLY place it ever appears), and the countdown
 * seconds until the session closes.
 */
void statusScreenPortalCredentials(const char* ssid, const char* password,
                                   uint32_t seconds_left);

/**
 * Securely wipe the static credential text buffers used by
 * statusScreenPortalCredentials. Call when leaving the credentials screen /
 * setup session so the one-time WPA2 password never lingers in status RAM.
 */
void statusScreenClearCredentials();

/** A submitted candidate credential is being trialed (portal is down). */
void statusScreenCandidateTesting();

/** The candidate failed; the same setup session is reopening. */
void statusScreenCandidateFailed();

/** A verified candidate is being persisted to flash. */
void statusScreenCommitting();

/**
 * Saved Wi-Fi could not connect and the device is offline. Explains the physical
 * button gesture to open setup; never auto-opens the portal.
 */
void statusScreenSavedWifiFailed();

/**
 * Fail-closed credential fault: a flash credential write could not be verified
 * and its rollback could not be verified either, so the stored Wi-Fi credential
 * is in an unknown state. The device refuses to reconnect or reopen the portal
 * (no false success); only the physical factory-erase gesture can recover it.
 */
void statusScreenCredentialFault();

/**
 * Fail-closed factory-erase-incomplete fault: an erase could not verifiably clear
 * every subsystem, so the device stays network-off. Explains that a second erase
 * gesture (or a power-cycle, which resumes the erase marker) retries. Persistent.
 */
void statusScreenEraseIncomplete();

/**
 * Truthful post-commit warning: the Wi-Fi credential saved, but one or more
 * non-credential settings (location / units / runways) could not be persisted.
 * Shown briefly before the radar reclaims the panel; the Wi-Fi credential is NOT
 * rolled back for a display-setting write failure.
 */
void statusScreenSettingsSaveFailed();

/** Progressive two-stage gesture prompt (configure / arm / confirm erase). */
void statusScreenButtonPrompt(core::ProvisionButtonPrompt prompt);

/**
 * Factory erase result. The outcome aggregates every cleared subsystem; the
 * screen claims a clean wipe ONLY when core::factoryEraseAllCleared(outcome) is
 * true, otherwise it shows a truthful incomplete/failure warning so the user can
 * retry. The device restarts afterward.
 */
void statusScreenFactoryErase(const core::FactoryEraseOutcome& outcome);
