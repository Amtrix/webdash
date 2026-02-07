sudo cp webdash-server.service.env.template webdash-server.service.env
sudo sed -i "s|placeholder_WEBDASH|${WEBDASH}|g" webdash-server.service.env

sudo cp webdash-server.service.template webdash-server.service
sudo sed -i "s|placeholder_WEBDASH|${WEBDASH}|g" webdash-server.service
