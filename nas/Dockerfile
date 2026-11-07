# JustRide NAS service: Python standard library only, no pip install.
FROM python:3.12-alpine
# Web app built into the image: the release's www zip (same version as the head unit's firmware).
# nas_build_image.sh passes the newest git tag. A web app in the ./www mount takes precedence.
ARG JR_VERSION=1.0.0-beta.1
WORKDIR /app
ADD https://github.com/tspannagel/JustRide/releases/download/v${JR_VERSION}/justride-www-${JR_VERSION}.zip /tmp/www.zip
RUN python3 -m zipfile -e /tmp/www.zip /app/builtin && rm /tmp/www.zip \
 && test -f /app/builtin/www/stats/index.html.gz \
 && mkdir -p /data /app/www && chown 1000:1000 /data
COPY justride_nas.py .
# Starts as root only to give the /data bind mount to JR_UID:JR_GID, then the service drops to that user.
ENV PYTHONUNBUFFERED=1 JR_UID=1000 JR_GID=1000
EXPOSE 8080
VOLUME /data
HEALTHCHECK --interval=60s --timeout=5s --start-period=20s \
  CMD python3 -c "import http.client as h; c=h.HTTPConnection('localhost', 8080, timeout=4); c.request('GET', '/api/info', headers={'Host': 'localhost'}); exit(c.getresponse().status != 200)"
CMD ["python3", "justride_nas.py"]
