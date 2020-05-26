# Sauna

An tool to measure energy the consumption of CPUs, GPUs and XeonPhi.

It performs a sequence of power measurements on the different devices during the execution of a given program. Output is made in tabular form, convenient for graphing. 

## Prerequisites

The energy measurements of the CPUs is done through the Running Average Power Limit (RAPL). Support for these was added to the Linux kernel since version 3.14.

To access the Nvidia GPUs and XeonPhi, Sauna uses two libraries provided by both manufacturers. From Nvidia, Sauna requires the [GDK](https://developer.nvidia.com/gpu-deployment-kit). And from Intel, the [XeonPhi Library]() must be installed.


## Building

The Makefile easies the building process. To build the complete Sauna, simply type:

```sh
$ make
```

Building of the Nvidia or XeonPhi modules can be disabled on the command line by setting the value of the corresponding variables, NVIDIA and XEONPHI. These changes can be made permanent by modifying the Makefile directly.

```sh
$ make XEONPHI=1 NVIDIA=0
```

## Running

At the very minimum Sauna takes one argument. It is the program to run while it performs the measurements. Without any arguments it shows a brief usage guide. A more detalied user guide can be obtained with '-h'.

Since Sauna requires access to protected devices, it must run with root privileges. This should show the power consumpition for 5 seconds.

```sh
$ sudo sauna sleep 5
```

The default sampling interval is 500ms. Other values can be set with '-i'. However be aware that using short intervals can impose a significant overhead. This is particularly noticeable in Nvidia devices. Evaluation of the overhead is recommended if the interval is lower than 100ms.

By default Sauna takes measurements throughout the execution, but this can be restricted to a \emph{Region Of Interest(ROI)} with '-r'. The ROI is determined by the program itself by special strings written to standard output. Care must be taken in this case to flush the output after printing these strings so that the monitor can read them as soon as possible.


## Authors

* [Esteban Stafford](http://personales.gestion.unican.es/stafforde/) - *Main design*
* [Borja Perez](https://www.atc.unican.es/members.html) - *Nvidia code*
* [Raúl Nozal](https://www.atc.unican.es/members.html) - *XeonPhi code*

## License

This project is licensed under GNU GPLv3 - see the [LICENSE.md](LICENSE.md) file for details.

## Acknowledgments

* RAPL code [Vince Weaver](http://web.eece.maine.edu/~vweaver/projects/rapl/)

