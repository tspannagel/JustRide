# JustRide on a NAS

Look at your stats and edit plans and workouts while the head unit is switched off.
The NAS keeps a copy of the head unit's rides, riders, workouts and plans, serves the
same web app, and hands your plan and workout changes to the head unit the next time it
is on (it checks about every minute).

What stays on the head unit:

- **Rides** are only recorded there; the NAS copies them.
- **Riders** (FTP, weight, Max ERG): changes go straight to the head unit and are refused
  while it is off.
- **Firmware and web-app updates** need the tap on the head unit, so they are done from the
  head unit's own page (`http://trainer.local/stats/#device`), not from the NAS.

If a plan was changed on both sides, the NAS copy wins and the head unit's copy is kept in
`data/conflicts/`.

## Setup

1. **Give the head unit a fixed address.** In your router, add a DHCP reservation for the
   head unit (its address is on the device's Wi-Fi screen). Example: `192.168.1.50`.
2. **Copy this folder to the NAS**, e.g. `/volume1/docker/justride/`.
3. **Add the web app.** Download `justride-www-<version>.zip` from the GitHub release and
   unzip it in that folder, so you have `www/stats/index.html.gz`. Use the same version as
   on the head unit.
4. **Set the address** in `compose.yaml` (`JR_DEVICE`). If you open the NAS by a name
   (e.g. `nas.local`) rather than its IP address, add that name to `JR_HOSTS`.
5. **Start it:**

   ```sh
   mkdir -p data && sudo chown 1000:1000 data
   docker compose up -d --build
   ```

6. Open `http://<nas-address>:8080/`. With the head unit off, a bar at the top says so
   and how old the data is.

On Synology/QNAP you can do steps 5 and 6 in Container Manager / Container Station with
"Create project" and this `compose.yaml`.

With Podman instead of Docker, build with `podman build --format docker -t justride-nas nas/`
(otherwise the health check in the Dockerfile is dropped) and run it with the same port and
volumes as in `compose.yaml`.

## Updating

- Web app: unzip the new release zip over `www/` (the head unit gets its copy separately,
  `tools/web_deploy.sh` or the Device page on the head unit).
- Service: copy the new `justride_nas.py` and run `docker compose up -d --build`.

## Safety and access

- Keep it on your home network. Don't forward the port on your router; use your NAS's or
  router's VPN to reach it from outside. HTTPS is optional through the NAS's reverse proxy.
- The NAS talks to the head unit like the web app does and can't do anything the web app
  couldn't: no firmware uploads, no rider changes while the head unit is off.

## Without Docker

```sh
python3 nas/justride_nas.py --device 192.168.1.50 --www ./www/stats --data ./data
```

Tests: `python3 -m unittest discover -s test/tools`.
