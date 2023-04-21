openssl genpkey -algorithm RSA -out main.key -pkeyopt rsa_keygen_bits:2048 &&
openssl req -new -key main.key -out srv.csr &&
openssl x509 -req -days 365 -in srv.csr -signkey main.key -out srv.crt &&
openssl req -new -x509 -key main.key -out cas.pem 