/*
 *  main.c
 *  This file is part of the ich9deblob utility from the libreboot project
 * 
 * Purpose: disable and remove the ME from ich9m/gm45 machines in coreboot.
 *
 *  Copyright (C) 2014 Steve Shenton <sgsit@libreboot.org>
 *                     Francis Rowe <info@gluglug.org.uk>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
 
/*
 * Read a factory.rom dump (ich9m/gm45 machines) and 
 * modify the flash descriptor to remove all regions except descriptor,
 * Gbe and BIOS. Set BIOS region to full size of the ROM image (after
 * the flash descriptor and gbe). Basically, deblob the descriptor.
 * 
 * This will will generate a concatenated descriptor+gbe dump suitable
 * for use in libreboot. Currently tested: ThinkPad X200 (coreboot/libreboot)
 */
 
// See docs/hcl/x200_remove_me.html for info plus links to datasheet (also linked below)

// Info about flash descriptor (read page 850 onwards):
// * http://www.intel.co.uk/content/dam/doc/datasheet/io-controller-hub-9-datasheet.pdf

// Info about Gbe region (read whole datasheet):
// * http://www.intel.co.uk/content/dam/doc/application-note/i-o-controller-hub-9m-82567lf-lm-v-nvm-map-appl-note.pdf
// * https://communities.intel.com/community/wired/blog/2010/10/14/how-to-basic-eeprom-checksums

#include <stdio.h>
#include <string.h>
#include "ich9desc.c" // structs describing what's in the descriptor region
#include "ich9gbe.c" // structs describing what's in the gbe region

#define DESCRIPTORREGIONSIZE 0x1000 // 4 KiB
#define GBEREGIONSIZE 0x2000 // 8 KiB
// These will have a modified descriptor+gbe based on what's in the factory.rom
// These will be joined into a single 12KiB buffer (descriptor, then gbe) and saved to a file
// NOTE: The GBE region of 8K is actually 2x 4K regions in a single region; both 4K blocks can be identical (and by default, are)
// The 2nd one is a "backup", but we don't know when it's used. perhaps it's used when the checksum on the first one does not match?

// Related to the flash descriptor
#define FLREGIONBITSHIFT 0xC // bits 12(0xC)-24(0x18) are represented for words found in the flash descriptor
												 // To manipulate these easily in C, we shift them by FLREGIONBITSHIFT and then shift them back when done

unsigned short gbeGetChecksumFrom4kStruct(struct GBEREGIONRECORD_4K gbeStruct4k, unsigned short desiredValue);
unsigned short gbeGetChecksumFrom8kBuffer(char* buffer, unsigned short desiredValue, char isBackup); // for GBe region (checksum calculation)
unsigned short gbeGetRegionWordFrom8kBuffer(int i, char* buffer); // used for getting each word needed to calculate said checksum
struct DESCRIPTORREGIONRECORD deblobbedDescriptorStructFromFactory(struct DESCRIPTORREGIONRECORD factoryDescriptorStruct, unsigned int romSize, unsigned int factoryGbeRegionLocation);
int structSizesIncorrect(struct DESCRIPTORREGIONRECORD descriptorDummy, struct GBEREGIONRECORD_8K gbe8kDummy);
int systemIsBigEndian();
int structBitfieldWrongOrder();
int structMembersWrongOrder();
struct GBEREGIONRECORD_8K deblobbedGbeStructFromFactory(struct GBEREGIONRECORD_8K factoryGbeStruct8k);
int systemOrCompilerIncompatible(struct DESCRIPTORREGIONRECORD descriptorStruct, struct GBEREGIONRECORD_8K gbeStruct8k);

int main(int argc, char *argv[])
{
	// descriptor region. Will have an actual descriptor struct mapped to it (from the factory.rom dump)
	// and then it will be modified (deblobbed) to remove the ME/AMT
	char factoryDescriptorBuffer[DESCRIPTORREGIONSIZE];
	struct DESCRIPTORREGIONRECORD factoryDescriptorStruct;
	char deblobbedDescriptorBuffer[DESCRIPTORREGIONSIZE];
	struct DESCRIPTORREGIONRECORD deblobbedDescriptorStruct;
	
	// gbe region. Well have actual gbe buffer mapped to it (from the factory.rom dump)
	// and then it will be modified to correct the main region
	char factoryGbeBuffer8k[GBEREGIONSIZE];
	struct GBEREGIONRECORD_8K factoryGbeStruct8k;
	char deblobbedGbeBuffer8k[GBEREGIONSIZE];
	struct GBEREGIONRECORD_8K deblobbedGbeStruct8k;
	
	// Used to store the location of the Gbe
	// region inside the factory.rom image.
	unsigned int factoryGbeRegionLocation;
	
	// names of the files that this utility will handle
	char* factoryRomFilename = "factory.rom"; // user-supplied factory.bin dump (original firmware)
	char* deblobbedDescriptorFilename = "deblobbed_descriptor.bin"; // descriptor+gbe: to be dd'd to beginning of a libreboot image
	
	// Used when reading the factory.rom to extract descriptor/gbe regions
	unsigned int bufferLength;
	
	// For storing the size of the factory.rom dump in bytes
	unsigned int romSize;
	
	// -----------------------------------------------------------------------------------------------
	
	// Compatibility checks. This version of ich9deblob is not yet porable.
	if (systemOrCompilerIncompatible(factoryDescriptorStruct, factoryGbeStruct8k)) return 1;
	
	// -----------------------------------------------------------------------------------------------

	// Open factory.rom, needed for extracting descriptor and gbe
	// -----------------------------------------------
	FILE* fileStream = NULL;
	fileStream = fopen(factoryRomFilename, "rb"); // open factory.rom
	if (NULL == fileStream)
	{
		printf("\nerror: could not open factory.rom\n");
		return 1;
	}
	printf("\nfactory.rom opened successfully\n");
	// -----------------------------------------------
	
	// Get the descriptor region dump from the factory.rom
	// (goes in factoryDescriptorBuffer variable)
	bufferLength = fread(factoryDescriptorBuffer, sizeof(char), DESCRIPTORREGIONSIZE, fileStream);
	if (DESCRIPTORREGIONSIZE != bufferLength) // 
	{
		printf("\nerror: could not read descriptor from factory.rom (%i) bytes read\n", bufferLength);
		return 1;
	}
	printf("\ndescriptor region read successfully\n");
	// copy descriptor buffer into descriptor struct memory
	// factoryDescriptorStruct is an instance of a struct that actually
	// defines the locations of all these variables in the descriptor,
	// as defined in the datasheets. This allows us to map the extracted
	// descriptor over the struct so that it can then be modified
	// for libreboot's purpose
	memcpy(&factoryDescriptorStruct, &factoryDescriptorBuffer, DESCRIPTORREGIONSIZE);
	// ^ the above is just for reference if needed. The modifications will be made here:
	memcpy(&deblobbedDescriptorStruct, &factoryDescriptorBuffer, DESCRIPTORREGIONSIZE);
	
	// -----------------------------------------------------------------------------------------------
	
	// Get the gbe region dump from the factory.rom

	// get original GBe region location
	// (it will be moved to the beginning of the flash, after the descriptor region)
	// note for example, factoryGbeRegionLocation is set to <<FLREGIONBITSHIFT of actual address (in C). this is how the addresses
	// are stored in the descriptor.
	factoryGbeRegionLocation = factoryDescriptorStruct.regionSection.flReg3.BASE << FLREGIONBITSHIFT;

	// Set offset so that we can read the data from
	// the gbe region
	fseek(fileStream, factoryGbeRegionLocation, SEEK_SET);
	// Read the gbe data from the factory.rom and put it in factoryGbeBuffer8k
	bufferLength = fread(factoryGbeBuffer8k, sizeof(char), GBEREGIONSIZE, fileStream);
	if (GBEREGIONSIZE != bufferLength)
	{
		printf("\nerror: could not read GBe region from factory.rom (%i) bytes read\n", bufferLength);
		return 1;
	}
	printf("\ngbe (8KiB) region read successfully\n");
	// copy gbe buffer into gbe struct memory
	// factoryGbeStruct8k is an instance of a struct that actually
	// defines the locations of all these variables in the gbe,
	// as defined in the datasheets. This allows us to map the extracted
	// gbe over the struct so that it can then be modified
	// for libreboot's purpose
	memcpy(&factoryGbeStruct8k, &factoryGbeBuffer8k, GBEREGIONSIZE);
	// the original factoryGbeStruct8k is only reference. Changes go here:
	memcpy(&deblobbedGbeStruct8k, &factoryGbeBuffer8k, GBEREGIONSIZE);

	// -----------------------------------------------------------------------------------------------

	// Get size of ROM image
	// This is needed for relocating the BIOS region (per descriptor)
	fseek(fileStream, 0L, SEEK_END);
	romSize = ftell(fileStream);

	printf("\nfactory.rom size: [%i] bytes\n", romSize);

	fclose(fileStream);
	
	// -----------------------------------------------------------------------------------------------

	// Disable the ME and Platform regions. Put Gbe at the beginning (after descriptor). 
	// Also, extend the BIOS region to fill the ROM image (after descriptor+gbe).
	deblobbedDescriptorStruct = deblobbedDescriptorStructFromFactory(factoryDescriptorStruct, romSize, factoryGbeRegionLocation);

	// ----------------------------------------------------------------------------------------------------------------

	// Modify the Gbe descriptor (see function for details)
	deblobbedGbeStruct8k = deblobbedGbeStructFromFactory(factoryGbeStruct8k);

	// ----------------------------------------------------------------------------------------------------------------

	// Convert the deblobbed descriptor and gbe back to byte arrays, so that they
	// can more easily be written to files:
	// deblobbed descriptor region
	memcpy(&deblobbedDescriptorBuffer, &deblobbedDescriptorStruct, DESCRIPTORREGIONSIZE); // descriptor
	memcpy(&deblobbedGbeBuffer8k, &deblobbedGbeStruct8k, GBEREGIONSIZE); // gbe

	// delete old file before continuing
	remove(deblobbedDescriptorFilename);
	// open new file for writing the deblobbed descriptor+gbe
	fileStream = fopen(deblobbedDescriptorFilename, "ab");

	// write the descriptor region into the first part
	if (DESCRIPTORREGIONSIZE != fwrite(deblobbedDescriptorBuffer, sizeof(char), DESCRIPTORREGIONSIZE, fileStream))
	{
		printf("\nerror: writing descriptor region failed\n");
		return 1;
	}

	// add gbe to the end of the file
	if (GBEREGIONSIZE != fwrite(deblobbedGbeBuffer8k, sizeof(char), GBEREGIONSIZE, fileStream))
	{
		printf("\nerror: writing GBe region failed\n");
		return 1;
	}

	fclose(fileStream);

	printf("\ndeblobbed descriptor successfully created: deblobbed_descriptor.bin \n");

	// -------------------------------------------------------------------------------------

	printf("\nNow do: dd if=deblobbed_descriptor.bin of=libreboot.rom bs=1 count=12k conv=notrunc");
	printf("\n(in other words, add the modified descriptor+gbe to your ROM image)\n");

	return 0;
}

// Basically, this should only return true on non-x86 machines
int structSizesIncorrect(struct DESCRIPTORREGIONRECORD descriptorDummy, struct GBEREGIONRECORD_8K gbe8kDummy) {
	unsigned int descriptorRegionStructSize = sizeof(descriptorDummy);
	unsigned int gbeRegion8kStructSize = sizeof(gbe8kDummy);
	// check compiler bit-packs in a compatible way. basically, it is expected that this code will be used on x86
	if (DESCRIPTORREGIONSIZE != descriptorRegionStructSize){
		printf("\nerror: compiler incompatibility: descriptor struct length is %i bytes (should be %i)\n", descriptorRegionStructSize, DESCRIPTORREGIONSIZE);
		return 1;
	}
	if (GBEREGIONSIZE != gbeRegion8kStructSize){
		printf("\nerror: compiler incompatibility: gbe struct length is %i bytes (should be %i)\n", gbeRegion8kStructSize, GBEREGIONSIZE);
		return 1;
	}
	return 0;
}

int systemIsBigEndian() {
	// endianness check. big endian forced to fail
	unsigned short steak = 0xBEEF;
	unsigned char *grill = (unsigned char*)&steak;
	if (*grill==0xBE) {
		printf("\nunsigned short 0xBEEF: first byte should be EF, but it's BE. Your system is big endian, and unsupported (only little endian is tested)\n");
		return 1;
	}
	return 0;
}

// fail if members are presented in the wrong order
int structMembersWrongOrder()
{
	int i;
	struct DESCRIPTORREGIONRECORD descriptorDummy;
	unsigned char *meVsccTablePtr = (unsigned char*)&descriptorDummy.meVsccTable;
	
	// These do not use bitfields. 
	descriptorDummy.meVsccTable.jid0 = 0x01020304;  // unsigned int 32-bit
	descriptorDummy.meVsccTable.vscc0 = 0x10203040; // unsigned int 32-bit
	descriptorDummy.meVsccTable.jid1 = 0x11223344;  // unsigned int 32-bit
	descriptorDummy.meVsccTable.vscc1 = 0x05060708; // unsigned int 32-bit
	descriptorDummy.meVsccTable.jid2 = 0x50607080;  // unsigned int 32-bit
	descriptorDummy.meVsccTable.vscc2 = 0x55667788; // unsigned int 32-bit
	descriptorDummy.meVsccTable.padding[0] = 0xAA;  // unsigned char 8-bit
	descriptorDummy.meVsccTable.padding[1] = 0xBB;  // unsigned char 8-bit
	descriptorDummy.meVsccTable.padding[2] = 0xCC;  // unsigned char 8-bit
	descriptorDummy.meVsccTable.padding[3] = 0xDD;  // unsigned char 8-bit
	
	// Look from the top down, and concatenate the unsigned ints but
	// with each unsigned in little endian order. 
	// Then, concatenate the unsigned chars in big endian order. (in the padding array)
	
	// combined, these should become:
	// 01020304 10203040 11223344 05060708 50607080 55667788 AA BB CC DD (ignore this. big endian. just working it out manually:)
	// 04030201 40302010 44332211 08070605 80706050 88776655 AA BB CC DD (ignore this. not byte-separated, just working it out:)
	// 04 03 02 01 40 30 20 10 44 33 22 11 08 07 06 05 80 70 60 50 88 77 66 55 AA BB CC DD <-- it should match this
	
	printf("\nStruct member order check (descriptorDummy.meVsccTable) with junk/dummy data:");
	printf("\nShould be: 04 03 02 01 40 30 20 10 44 33 22 11 08 07 06 05 80 70 60 50 88 77 66 55 aa bb cc dd ");
	printf("\nAnd it is: ");
	
	for (i = 0; i < 28; i++) {
		printf("%02x ", *(meVsccTablePtr + i));	
	}
	printf("\n");
	
	if (
			!
			(
				*meVsccTablePtr      == 0x04 && *(meVsccTablePtr+1)  == 0x03 && *(meVsccTablePtr+2)  == 0x02 && *(meVsccTablePtr+3)  == 0x01
			&& *(meVsccTablePtr+4)  == 0x40 && *(meVsccTablePtr+5)  == 0x30 && *(meVsccTablePtr+6)  == 0x20 && *(meVsccTablePtr+7)  == 0x10
			&& *(meVsccTablePtr+8)  == 0x44 && *(meVsccTablePtr+9)  == 0x33 && *(meVsccTablePtr+10) == 0x22 && *(meVsccTablePtr+11) == 0x11
			&& *(meVsccTablePtr+12) == 0x08 && *(meVsccTablePtr+13) == 0x07 && *(meVsccTablePtr+14) == 0x06 && *(meVsccTablePtr+15) == 0x05
			&& *(meVsccTablePtr+16) == 0x80 && *(meVsccTablePtr+17) == 0x70 && *(meVsccTablePtr+18) == 0x60 && *(meVsccTablePtr+19) == 0x50
			&& *(meVsccTablePtr+20) == 0x88 && *(meVsccTablePtr+21) == 0x77 && *(meVsccTablePtr+22) == 0x66 && *(meVsccTablePtr+23) == 0x55
			&& *(meVsccTablePtr+24) == 0xAA && *(meVsccTablePtr+25) == 0xBB && *(meVsccTablePtr+26) == 0xCC && *(meVsccTablePtr+27) == 0xDD
	      )
	   ) {
		printf("Incorrect order.\n");
		return 1;
	}
	printf("Correct order.\n");
	return 0;
}

// fail if bit fields are presented in the wrong order
int structBitfieldWrongOrder() 
{
	int i;
	struct DESCRIPTORREGIONRECORD descriptorDummy;
	unsigned char *flMap0Ptr = (unsigned char*)&descriptorDummy.flMaps.flMap0;
	
	descriptorDummy.flMaps.flMap0.FCBA = 0xA2;      // :8 --> 10100010
	descriptorDummy.flMaps.flMap0.NC = 0x02;        // :2 --> 10
	descriptorDummy.flMaps.flMap0.reserved1 = 0x38; // :6 --> 111000
	descriptorDummy.flMaps.flMap0.FRBA = 0xD2;      // :8 --> 11010010
	descriptorDummy.flMaps.flMap0.NR = 0x05;        // :3 --> 101
	descriptorDummy.flMaps.flMap0.reserved2 = 0x1C; // :5 --> 11100
	
	// Look from the top bottom up, and concatenate the binary strings.
	// Then, convert the 8-bit groups to hex and reverse the (8-bit)byte order
	
	// combined, these should become (in memory), in binary:
	// 10100010 11100010 11010010 11100101
	// or in hex:
	// A2 E2 D2 E5
	
	printf("\nBitfield order check (descriptorDummy.flMaps.flMaps0) with junk/dummy data:");
	printf("\nShould be: a2 e2 d2 e5 ");
	printf("\nAnd it is: ");

	for (i = 0; i < 4; i++) {
		printf("%02x ", *(flMap0Ptr + i));	
	}
	printf("\n");
	
	if (!(*flMap0Ptr == 0xA2 && *(flMap0Ptr+1) == 0xE2 && *(flMap0Ptr+2) == 0xD2 && *(flMap0Ptr+3) == 0xE5)) {
		printf("Incorrect order.\n");
		return 1;
	}
	printf("Correct order.\n");
	return 0;
}

// Compatibility checks. This version of ich9deblob is not yet porable.
int systemOrCompilerIncompatible(struct DESCRIPTORREGIONRECORD descriptorStruct, struct GBEREGIONRECORD_8K gbeStruct8k) 
{
	if (structSizesIncorrect(descriptorStruct, gbeStruct8k)) return 1;
	if (systemIsBigEndian()) return 1;
	if (structBitfieldWrongOrder()) return 1;
	if (structMembersWrongOrder()) return 1; 
	return 0;
}

struct GBEREGIONRECORD_8K deblobbedGbeStructFromFactory(struct GBEREGIONRECORD_8K factoryGbeStruct8k) 
{	
	// Correct the main gbe region. By default, the X200 (as shipped from Lenovo) comes
	// with a broken main gbe region, where the backup gbe region is used instead. Modify
	// the descriptor so that the main region is usable.
	
	struct GBEREGIONRECORD_8K deblobbedGbeStruct8k;
	memcpy(&deblobbedGbeStruct8k, &factoryGbeStruct8k, GBEREGIONSIZE);
	
	deblobbedGbeStruct8k.backup.checkSum = gbeGetChecksumFrom4kStruct(deblobbedGbeStruct8k.backup, 0xBABA);
	memcpy(&deblobbedGbeStruct8k.main, &deblobbedGbeStruct8k.backup, GBEREGIONSIZE>>1);
	
	// Debugging:
	// calculate the 0x3F'th 16-bit uint to make the desired final checksum for GBe
	// observed checksum matches (from X200 factory.rom dumps) on main: 0x3ABA 0x34BA 0x40BA. spec defined as 0xBABA.
	// X200 ships with a broken main gbe region by default (invalid checksum, and more)
	// The "backup" gbe regions on these machines are correct, though, and is what the machines default to
	// For libreboot's purpose, we can do much better than that by fixing the main one... below is only debugging
	printf("\nfactory Gbe (main): calculated Gbe checksum: 0x%hx and actual GBe checksum: 0x%hx\n", gbeGetChecksumFrom4kStruct(factoryGbeStruct8k.main, 0xBABA), factoryGbeStruct8k.main.checkSum);
	printf("factory Gbe (backup) calculated Gbe checksum: 0x%hx and actual GBe checksum: 0x%hx\n", gbeGetChecksumFrom4kStruct(factoryGbeStruct8k.backup, 0xBABA), factoryGbeStruct8k.backup.checkSum);
	printf("\ndeblobbed Gbe (main): calculated Gbe checksum: 0x%hx and actual GBe checksum: 0x%hx\n", gbeGetChecksumFrom4kStruct(deblobbedGbeStruct8k.main, 0xBABA), deblobbedGbeStruct8k.main.checkSum);
	printf("deblobbed Gbe (backup) calculated Gbe checksum: 0x%hx and actual GBe checksum: 0x%hx\n", gbeGetChecksumFrom4kStruct(deblobbedGbeStruct8k.backup, 0xBABA), deblobbedGbeStruct8k.backup.checkSum);
	
	return deblobbedGbeStruct8k;
}

// Modify the flash descriptor, to remove the ME/AMT, and disable all other regions
// Only Flash Descriptor, Gbe and BIOS regions (BIOS region fills romSize-12k) are left.
// Tested on ThinkPad X200 and X200S. X200T and other GM45 targets may also work.
struct DESCRIPTORREGIONRECORD deblobbedDescriptorStructFromFactory(struct DESCRIPTORREGIONRECORD factoryDescriptorStruct, unsigned int romSize, unsigned int factoryGbeRegionLocation)
{
	struct DESCRIPTORREGIONRECORD deblobbedDescriptorStruct;
	memcpy(&deblobbedDescriptorStruct, &factoryDescriptorStruct, DESCRIPTORREGIONSIZE);
	
	// Now we need to modify the descriptor so that the ME can be excluded
	// from the final ROM image (libreboot one) after adding the modified
	// descriptor+gbe. Refer to libreboot docs for details: docs/hcl/x200_remove_me.html

	// set number of regions from 4 -> 2 (0 based, so 4 means 5 and 2
	// means 3. We want 3 regions: descriptor, gbe and bios, in that order)
	deblobbedDescriptorStruct.flMaps.flMap0.NR = 2;

	// make descriptor writable from OS. This is that the user can run:
	// sudo ./flashrom -p internal:laptop=force_I_want_a_brick 
	// from the OS, without relying an an external SPI flasher, while
	// being able to write to the descriptor region (locked by default,
	// until making the change below):
	deblobbedDescriptorStruct.masterAccessSection.flMstr1.fdRegionWriteAccess = 1;

	// relocate BIOS region and increase size to fill image
	deblobbedDescriptorStruct.regionSection.flReg1.BASE = 3; // 3<<FLREGIONBITSHIFT is 12KiB, which is where BIOS region is to begin (after descriptor and gbe)
	deblobbedDescriptorStruct.regionSection.flReg1.LIMIT = ((romSize >> FLREGIONBITSHIFT) - 1);
	// ^ for example, 8MB ROM, that's 8388608 bytes.
	// ^ 8388608>>FLREGIONBITSHIFT (or 8388608/4096) = 2048 bytes
	// 2048 - 1 = 2047 bytes. 
	// This defines where the final 0x1000 (4KiB) page starts in the flash chip, because the hardware does:
	// 2047<<FLREGIONBITSHIFT (or 2047*4096) = 8384512 bytes, or 7FF000 bytes
	// (it can't be 0x7FFFFF because of limited number of bits)

	// set ME region size to 0 - the ME is a blob, we don't want it in libreboot
	deblobbedDescriptorStruct.regionSection.flReg2.BASE = 0x1FFF; // setting 1FFF means setting size to 0. 1FFF<<FLREGIONBITSHIFT is outside of the ROM image (8MB) size?
	// ^ datasheet says to set this to 1FFF, but FFF was previously used and also worked.
	deblobbedDescriptorStruct.regionSection.flReg2.LIMIT = 0;
	// ^ 0<<FLREGIONBITSHIFT=0, so basically, the size is 0, and the base (1FFF>>FLREGIONBITSHIFT) is well outside the higher 8MB range. 
	
	// relocate Gbe region to begin at 4KiB (immediately after the flash descriptor)
	deblobbedDescriptorStruct.regionSection.flReg3.BASE = 1; // 1<<FLREGIONBITSHIFT is 4096, which is where the Gbe region is to begin (after the descriptor)
	deblobbedDescriptorStruct.regionSection.flReg3.LIMIT = 2;
	// ^ 2<<FLREGIONBITSHIFT=8192 bytes. So we are set it to size 8KiB after the first 4KiB in the flash chip.

	// set Platform region size to 0 - another blob that we don't want
	deblobbedDescriptorStruct.regionSection.flReg4.BASE = 0x1FFF; // setting 1FFF means setting size to 0. 1FFF<<FLREGIONBITSHIFT is outside of the ROM image (8MB) size?
	// ^ datasheet says to set this to 1FFF, but FFF was previously used and also worked.
	deblobbedDescriptorStruct.regionSection.flReg4.LIMIT = 0;
	// ^ 0<<FLREGIONBITSHIFT=0, so basically, the size is 0, and the base (1FFF>>FLREGIONBITSHIFT) is well outside the higher 8MB range.

	// disable ME in ICHSTRAP0 - the ME is a blob, we don't want it in libreboot
	deblobbedDescriptorStruct.ichStraps.ichStrap0.meDisable = 1;

	// disable ME and TPM in MCHSTRAP0
	deblobbedDescriptorStruct.mchStraps.mchStrap0.meDisable = 1; // ME is a blob. not wanted in libreboot.
	deblobbedDescriptorStruct.mchStraps.mchStrap0.tpmDisable = 1; // not wanted in libreboot

	// disable ME, apart from chipset bugfixes (ME region should first be re-enabled above)
	// This is sort of like the CPU microcode updates, but for the chipset
	// (commented out below here, since blobs go against libreboot's purpose,
	// but may be interesting for others)
	// deblobbedDescriptorStruct.mchStraps.mchStrap0.meAlternateDisable = 1;
	
	// debugging
	printf("\nOriginal (factory.rom) Descriptor start block: %08x ; Descriptor end block: %08x\n", factoryDescriptorStruct.regionSection.flReg0.BASE << FLREGIONBITSHIFT, factoryDescriptorStruct.regionSection.flReg0.LIMIT << FLREGIONBITSHIFT);
	printf("Original (factory.rom) BIOS start block: %08x ; BIOS end block: %08x\n", factoryDescriptorStruct.regionSection.flReg1.BASE << FLREGIONBITSHIFT, factoryDescriptorStruct.regionSection.flReg1.LIMIT << FLREGIONBITSHIFT);
	printf("Original (factory.rom) ME start block: %08x ; ME end block: %08x\n", factoryDescriptorStruct.regionSection.flReg2.BASE << FLREGIONBITSHIFT, factoryDescriptorStruct.regionSection.flReg2.LIMIT << FLREGIONBITSHIFT);
	printf("Original (factory.rom) GBe start block: %08x ; GBe end block: %08x\n", factoryGbeRegionLocation, factoryDescriptorStruct.regionSection.flReg3.LIMIT << FLREGIONBITSHIFT);
	
	printf("\nRelocated (libreboot.rom) Descriptor start block: %08x ; Descriptor end block: %08x\n", deblobbedDescriptorStruct.regionSection.flReg0.BASE << FLREGIONBITSHIFT, deblobbedDescriptorStruct.regionSection.flReg0.LIMIT << FLREGIONBITSHIFT);
	printf("Relocated (libreboot.rom) BIOS start block: %08x ; BIOS end block: %08x\n", deblobbedDescriptorStruct.regionSection.flReg1.BASE << FLREGIONBITSHIFT, deblobbedDescriptorStruct.regionSection.flReg1.LIMIT << FLREGIONBITSHIFT);
	printf("Relocated (libreboot.rom) ME start block: %08x ; ME end block: %08x\n", deblobbedDescriptorStruct.regionSection.flReg2.BASE << FLREGIONBITSHIFT, deblobbedDescriptorStruct.regionSection.flReg2.LIMIT << FLREGIONBITSHIFT);
	printf("Relocated (libreboot.rom) GBe start block: %08x ; GBe end block: %08x\n", deblobbedDescriptorStruct.regionSection.flReg3.BASE << FLREGIONBITSHIFT, deblobbedDescriptorStruct.regionSection.flReg3.LIMIT << FLREGIONBITSHIFT);
	
	return deblobbedDescriptorStruct;
}

// checksum calculation for 4k gbe struct (algorithm based on datasheet)
unsigned short gbeGetChecksumFrom4kStruct(struct GBEREGIONRECORD_4K gbeStruct4k, unsigned short desiredValue)
{
	char gbeBuffer4k[GBEREGIONSIZE>>1];
	memcpy(&gbeBuffer4k, &gbeStruct4k, GBEREGIONSIZE>>1);
	return gbeGetChecksumFrom8kBuffer(gbeBuffer4k, desiredValue, 0);
}
// checksum calculation for 8k gbe region (algorithm based on datasheet)
// also works for 4k buffers, so long as isBackup remains false
unsigned short gbeGetChecksumFrom8kBuffer(char* regionData, unsigned short desiredValue, char isBackup)
{
	int i;
	
	unsigned short regionWord; // store words here for adding to checksum
	unsigned short checksum = 0; // this gbe's checksum
	unsigned short offset = 0; // in bytes, from the start of the gbe region.
	
	// if isBackup is true, use 2nd gbe region ("backup" region)
	if (isBackup) offset = 0x1000>>1; // this function uses *word* not *byte* indexes.

	for (i = 0; i < 0x3F; i++) {
		regionWord = gbeGetRegionWordFrom8kBuffer(i+offset, regionData);
		checksum += regionWord;
	}
	checksum = desiredValue - checksum;
	return checksum;
}
// Read a 16-bit unsigned int from a supplied region buffer
unsigned short gbeGetRegionWordFrom8kBuffer(int index, char* regionData)
{
	return *((unsigned short*)(regionData + (index * 2)));
}
