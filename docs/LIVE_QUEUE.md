# Live queue while preparing requests

1. Connect the FLX4 and headphones. In **Settings → Audio Output**, choose the
   speaker output and test the FLX4 phones. The live queue requires this separate
   headphone route; a laptop-only stereo output cannot keep preparation private.
2. Open **Transitions → Live Queue / Record Set** (also in the library).
   Open a saved set, enter **From song list**, or add transitions in order.
   Resolve any missing/ambiguous audio choices and prepare required stems.
3. Click **START LIVE QUEUE**. It plays the first song from its beginning,
   performs the saved transitions, bridges solo-section BPM/EQ, then plays the
   final song to its end. Speaker music is independent of the two main decks.
4. The main decks, controller and editor now prepare audio through FLX4 phones.
   Use **File → Add request audio** to prioritize a newly downloaded file without
   rescanning everything. Load it, inspect its grid, make cues and prepare a
   connecting transition normally. Editor preview is also headphones-only.
5. Extend the queue list and click **APPEND to live queue**. Accepted entries
   must stay unchanged. Wait for the acceptance message; do not assume the
   editable list has already changed the live set. Save your local set plan if
   you want to reuse it later.

Alternating PLAY/CUE lights and the PREP badge mean musical controller actions
affect preparation only. Hardware master/headphone volume knobs can still change
their physical outputs. This is not a hardware mute or protection against
unplugging the speaker cable.

The queue buffers up to 30 seconds ahead. Append early: already-rendered audio
cannot be changed, and late entries are refused. This version supports appending,
not skipping or replacing accepted songs. A missing connecting transition is
reported rather than replaced with an invented mix.

Closing the queue window does not stop music. **Pause live** and **Stop live**
affect speakers but keep preparation private. At the end, stop preparation decks
and editor preview, then use **Return decks to MASTER** explicitly. Disconnecting
phones never sends their preparation audio to the speakers.

First-release caution: automated routing/playback tests pass, but check the real
FLX4 outputs and lights at low volume before a gig. Heavy decoding/stem workloads
can still exhaust the buffer; its size and any underruns are shown in the window.
