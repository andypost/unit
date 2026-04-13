import email


def application(environ, start_response):
    content_type = environ.get('CONTENT_TYPE', '')
    content_length = int(environ.get('CONTENT_LENGTH', 0))
    body = environ['wsgi.input'].read(content_length)

    msg_data = f"Content-Type: {content_type}\r\n\r\n".encode() + body
    msg = email.message_from_bytes(msg_data)

    filename = ""
    file_data = b""

    for part in msg.walk():
        if part.get_content_disposition() == 'form-data':
            name = part.get_param('name', header='content-disposition')
            if name == 'file':
                filename = part.get_filename() or ""
                file_data = part.get_payload(decode=True) or b""
                break

    data = filename.encode() + file_data

    start_response(
        '200 OK',
        [('Content-Type', 'text/plain'), ('Content-Length', str(len(data)))],
    )

    return [data]
