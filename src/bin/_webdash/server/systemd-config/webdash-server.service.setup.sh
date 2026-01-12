cp webdash-server.service.env.template webdash-server.service.env
sed -i "s|placeholder_WEBDASH|${WEBDASH}|g" webdash-server.service.env

cp webdash-server.service.template webdash-server.service
sed -i "s|placeholder_WEBDASH|${WEBDASH}|g" webdash-server.service
