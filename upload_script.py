Import("env")

# The gateway answers HTTP 200 with body "OK" or "FAIL"; only "OK" counts as a successful upload.
env.Replace(UPLOADCMD="curl -sS -F image=@$SOURCE $UPLOAD_PORT | tee /dev/stderr | grep -qx OK")
