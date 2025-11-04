savedcmd_/app/movidius_x_vpu.mod := printf '%s\n'   movidius_x_vpu.o | awk '!x[$$0]++ { print("/app/"$$0) }' > /app/movidius_x_vpu.mod
