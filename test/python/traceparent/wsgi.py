def application(environ, start_response):
    """Echoes the inbound HTTP_TRACEPARENT (if any) back as a response
    header, so the test can see exactly what the app process received in
    its request environment -- not just what Unit's own response carries.
    """

    traceparent = environ.get('HTTP_TRACEPARENT', '')

    start_response(
        '200',
        [
            ('Content-Length', '0'),
            ('X-Seen-Traceparent', traceparent),
        ],
    )
    return []
