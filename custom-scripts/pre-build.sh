cp $BASE_DIR/../custom-scripts/S41network-config $BASE_DIR/target/etc/init.d
chmod +x $BASE_DIR/target/etc/init.d/S41network-config

cp $BASE_DIR/../apps/hello $BASE_DIR/target/usr/bin
cp $BASE_DIR/../custom-scripts/hello $BASE_DIR/target/etc/init.d/S50hello

chmod +x $BASE_DIR/target/etc/init.d/S50hell

cp $BASE_DIR/../custom-scripts/start-httpd $BASE_DIR/target/etc/init.d/S60httpd

chmod +x $BASE_DIR/target/etc/init.d/S60httpd

make -C $BASE_DIR/../modules/clook/

$BASE_DIR/../output/host/bin/i686-linux-gcc $BASE_DIR/../apps/clook-teste.c -o $BASE_DIR/target/usr/bin/clook-teste
