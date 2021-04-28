const {spawnSync} = require('child_process');

main().catch(e => {
  throw e;
});

async function main() {
  console.log('Building distribution binary...'); // eslint-disable-line no-console

  const prebuildArch = getNodearch(process.env.ARCH);

  // napi is forward compatible so we only build for two targets (Node and Electron)
  let prebuildScript = `prebuildify --napi --arch=${prebuildArch} -t 12.0.0 -t electron@9.4.4 --strip`;

  if (process.platform == 'linux') {
    prebuildScript = `${prebuildScript} --tag-libc`;
  }

  spawnSync(prebuildScript, {
    shell: true,
    stdio: 'inherit',
    encoding: 'utf8',
  });
}

/**
 * @param {string | undefined} arch the given architecture
 * @returns {string} the Node architecture to build for
 */
function getNodearch(arch) {
  if (!arch) {
    return process.arch;
  }
  if (arch === 'x86') {
    return 'ia32';
  } else {
    return arch;
  }
}
