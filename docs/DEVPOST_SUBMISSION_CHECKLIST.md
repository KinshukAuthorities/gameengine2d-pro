# OpenAI Build Week Submission Checklist

This checklist is based on the OpenAI Build Week submission requirements retrieved through the Devpost Hackathons integration on 2026-07-18.

## Required Devpost information

- [ ] **Submitter type** selected.
- [ ] **Country of residence** selected.
- [ ] **Category:** choose **Developer Tools** for GameEngine2D Pro.
- [ ] **Repository URL** supplied. If the repository is private, grant access to `testing@devpost.com` and `build-week-event@openai.com`.
- [ ] **Repository ownership:** keep the repository private; do not publish the proprietary source merely for the submission.
- [ ] **README** is present and includes setup, test, and Codex/GPT-5.6 information.
- [ ] **Public YouTube demo** is under three minutes and explains what was built, how Codex was used, and how GPT-5.6 was used.
- [x] **Codex Session ID:** `019f6664-fcff-7812-8dce-f3910c4b6615` is confirmed for the core project conversation; paste it into Devpost's Session ID field.
- [ ] **Submission status:** submitted, not draft.

## Developer-tool testing path for judges

1. Download `GameEngine2DPro-Setup-1.0.0-x64.exe` from the private repository's `v1.0.0` GitHub Release and install it on Windows x64.
2. Launch GameEngine Hub from the Start Menu.
3. Create an **Abyss of Hollows** sample project in the Hub.
4. Open it in the Editor and press Play.
5. Save one existing C++ script and select **Reload Scripts** to validate the native-script workflow.
6. Use Project Settings -> Build Settings to create a standalone export.

## Before pressing Submit

- [ ] Confirm that the public/private repository URL is correct and the latest README is committed.
- [ ] Create a private `v1.0.0` GitHub Release and attach the 526 MB installer; do not commit the installer binary directly to Git.
- [ ] Invite `testing@devpost.com` and `build-week-event@openai.com` to the private repository, then confirm both invitations are pending or accepted in GitHub's access page.
- [ ] Confirm the demo link works in an incognito browser.
- [x] Confirmed `/feedback` Session ID: `019f6664-fcff-7812-8dce-f3910c4b6615`.
- [ ] Confirm no secrets, personal access tokens, or private credentials are committed to the repository.
- [ ] Confirm the installer opens and GameEngine Hub starts from a clean user account if possible.
