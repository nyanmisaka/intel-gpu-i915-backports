#  Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported i915 Source Code of intel GPUs on various OSV Kernels.
You can create Dynamic Kernel Module Support(DKMS) based packages, which can be installed on supported OS distributions.

These out of tree i915 kernel module source codes are generated by the backpoprt project.
"https://backports.wiki.kernel.org/index.php/Main_Page" 

This repo is a code snapshot of particular version of backports and does not contain individual git change history
# Dependencies

This driver is part of a collection of kernel-mode drivers that enable support for Intel graphics. The backports collection within https://github.com/intel-gpu includes:

- [Intel® Graphics Driver Backports for Linux](https://github.com/intel-gpu/intel-gpu-i915-backports) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
- [Intel® Converged Security Engine Backports](https://github.com/intel-gpu/intel-gpu-cse-backports) - Converged Security Engine
- [Intel® Platform Monitoring Technology Backports](https://github.com/intel-gpu/intel-gpu-pmt-backports/) - Intel Platform Telemetry
- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-i915-backports) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag. 

# Supported OS Distributions


|   OSV |Branch   	| installation instructions | 
|---	|---	| --- |
| Red Hat® Enterprise Linux® 8.6 	|  [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/README.md)|
| Red Hat® Enterprise Linux® 8.5 	|  [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/README.md)| 
| SUSE® Linux® Enterprise Server 15SP3	| [suse/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/suse/main) |[Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/suse/main/README.md)|
| Ubuntu® 22.04 (linux-oem image 5.17) 	|[ubuntu/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/ubuntu/main)| [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/ubuntu/main/README.md)|
| Ubuntu® 20.04 (linux-oem image 5.14) 	|[ubuntu/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/ubuntu/main)| [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/ubuntu/main/README.md)|





