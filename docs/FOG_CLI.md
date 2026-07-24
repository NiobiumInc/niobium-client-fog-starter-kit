# The `fog` CLI reference

`fog` is the front-end to the Fog's jobs-as-a-service API. It ships with the niobium-client submodule at `niobium-client/scripts/fog` as a single stdlib-only Python script; the one extra it needs is a CA bundle for TLS (`certifi`, installed into the kit's `.venv` by `scripts/setup.sh`).

You use it to authenticate and to submit your workload so its run happens on a Fog worker. See the [README Quickstart, step 4](../README.md#4-run-it-on-the-fog).

## Subcommands

| Command | What it does |
|---|---|
| `fog init` | Creates `~/.fog/config` with the default keys (`api_url`, `mode`, `wait`, `maxwait`); it won't overwrite an existing file without `--force`. It writes the CLI's built-in defaults, so it is optional; use it when you want to customize `mode` or the long-poll windows. |
| `fog login [-u you@co]` | Logs in with your [console](https://console.niobium.co) email and password (prompted), provisions an API token, and saves it to `~/.fog/credentials`. Re-run anytime to rotate the token. |
| `fog submit <cmd> [args...]` | Provisions a job (`POST /jobs`), waits for a worker, wires `NBCC_FHETCH_SERVER` and `NBCC_FHETCH_TOKEN` into the environment, then `exec`s your command so its run streams to that worker. Pass your workload's `--target FOG` inside the command. |
| `fog list` | Table of all your jobs (id, status, mode, target, worker). |
| `fog get <id> [<id>...]` | Full JSON detail for specific jobs. |
| `fog cancel <id> [<id>...]` | Cancels or releases specific jobs. |
| `fog cancel --pending` | Cancels every in-flight job (frees workers you're holding). |

## Invoking it

With the kit's venv active, `scripts/setup.sh` links the CLI onto your PATH as plain `fog <cmd>`; that's what the examples here use. The full path `niobium-client/scripts/fog <cmd>` always works too, for example before you've activated the venv or from a standalone client (see [`USING_THE_CLIENT.md`](USING_THE_CLIENT.md)). For `fog` in every shell without activating the venv, add the client's `scripts/` dir to your PATH in `~/.bashrc` / `~/.zshrc`, pointed at wherever you cloned the kit:

```bash
export PATH="$HOME/niobium-client-fog-starter-kit/niobium-client/scripts:$PATH"
```

## Config and credentials

| File | Contents |
|---|---|
| `~/.fog/config` | Optional. `[fog]` section: `api_url` (default `https://api.niobium.co`), `mode` (`batch`), `wait` / `maxwait` long-poll seconds. Absent keys fall back to the CLI's built-in defaults, which are the same values. |
| `~/.fog/credentials` | `[fog] api_token = ...`, written by `fog login`. Keep it secret; never commit it. |

`scripts/setup.sh` creates the `.venv` (with `certifi`); after that you only log in:

```bash
scripts/setup.sh                          # venv + certifi (once)
source .venv/bin/activate                 # activates the venv; puts `fog` on PATH

fog login -u you@yourcompany.com
fog list                                  # confirm auth (an empty list is fine)
```

## Environment overrides

These win over `~/.fog/` and are handy for CI or one-off endpoint switches:

| Var | Overrides |
|---|---|
| `FOG_API_URL` | the API base (e.g. a staging endpoint) |
| `FOG_API_TOKEN` | the API token (sent as the `X-Api-Token` header) |
| `FOG_JOB_MODE` | the job mode (`batch`, ...) |
| `FOG_HOME` | the config directory (default `~/.fog`) |

## What the errors mean

| Code | Meaning | Fix |
|---|---|---|
| 401 unauthorized | token missing or expired | run `fog login` again |
| 403 forbidden | account not provisioned for that `--target` | contact Niobium |
| 429 over quota | too many in-flight jobs | `fog cancel --pending`, wait, or contact Niobium |

The [README Troubleshooting](../README.md#troubleshooting) table covers the rest.
