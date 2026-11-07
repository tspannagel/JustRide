# JustRide NAS service: Python standard library only, no pip install.
FROM python:3.12-alpine
WORKDIR /app
COPY justride_nas.py .
# The web app (release zip, www/stats) and the data volume are mounted, see compose.yaml.
RUN mkdir -p /data /app/www && chown 1000:1000 /data
USER 1000:1000
ENV PYTHONUNBUFFERED=1
EXPOSE 8080
VOLUME /data
HEALTHCHECK --interval=60s --timeout=5s --start-period=20s \
  CMD python3 -c "import http.client as h; c=h.HTTPConnection('localhost', 8080, timeout=4); c.request('GET', '/api/info', headers={'Host': 'localhost'}); exit(c.getresponse().status != 200)"
CMD ["python3", "justride_nas.py"]
